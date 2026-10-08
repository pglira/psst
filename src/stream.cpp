#include "stream.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

namespace {
constexpr int   kFrameMs       = 20;
constexpr int   kMinSpeechMs   = 120;   // shorter sounds count as noise
constexpr int   kPadMs         = 200;   // audio kept around each utterance
constexpr float kSpeechRatio   = 4.0f;  // speech is ~12 dB above the noise floor
constexpr float kMinSpeechRms  = 3e-4f; // ~-70 dBFS
constexpr float kDigitalSilence = 1e-6f;
constexpr float kNoiseRise     = 1.002f; // ~0.9 dB/s
constexpr float kNoiseFall     = 0.8f;
}

UtteranceSplitter::UtteranceSplitter(int sample_rate, int pause_ms, int max_ms)
    : frame_len_(static_cast<size_t>(sample_rate) * kFrameMs / 1000),
      pause_frames_(std::max(1, pause_ms / kFrameMs)),
      max_frames_(std::max(2, max_ms / kFrameMs)),
      min_speech_frames_(kMinSpeechMs / kFrameMs),
      pad_(static_cast<size_t>(sample_rate) * kPadMs / 1000) {}

std::vector<UtteranceSplitter::Range>
UtteranceSplitter::feed(const float* data, size_t count) {
    std::vector<Range> out;
    for (size_t i = 0; i < count; ++i) {
        partial_.push_back(data[i]);
        if (partial_.size() < frame_len_) continue;

        double sum = 0.0;
        for (float s : partial_) sum += double(s) * s;
        partial_.clear();
        on_frame(static_cast<float>(std::sqrt(sum / frame_len_)), out);
    }
    return out;
}

std::vector<UtteranceSplitter::Range> UtteranceSplitter::flush() {
    std::vector<Range> out;
    if (in_speech_ && (speech_frames_ >= min_speech_frames_ ||
                       (after_cut_ && speech_frames_ > 0)))
        emit(frames_ * frame_len_ + partial_.size(), out);
    in_speech_ = false;
    return out;
}

void UtteranceSplitter::on_frame(float rms, std::vector<Range>& out) {
    size_t frame = frames_++;

    // The noise floor follows quiet frames quickly and loud frames slowly.
    // Digital silence (e.g. from a source that is still starting) is not
    // noise and leaves the floor as it is.
    if (rms >= kDigitalSilence) {
        if (noise_ < 0.0f)
            noise_ = rms;
        else if (rms < noise_)
            noise_ = kNoiseFall * noise_ + (1.0f - kNoiseFall) * rms;
        else
            noise_ *= kNoiseRise;
    }

    bool speech = noise_ >= 0.0f && rms > kMinSpeechRms && rms > kSpeechRatio * noise_;

    if (!in_speech_) {
        if (!speech) return;
        in_speech_ = true;
        after_cut_ = false;
        speech_begin_ = frame;
        speech_frames_ = 0;
        levels_.clear();
        voiced_.clear();
    }

    levels_.push_back(rms);
    voiced_.push_back(speech);
    if (speech) {
        speech_last_ = frame;
        ++speech_frames_;
    }

    if (frame - speech_last_ >= pause_frames_) {
        if (speech_frames_ >= min_speech_frames_ || (after_cut_ && speech_frames_ > 0))
            emit((speech_last_ + 1) * frame_len_ + pad_, out);
        in_speech_ = false;
        return;
    }

    if (levels_.size() >= max_frames_) {
        // Cut at the quietest frame of the second half and continue after it.
        size_t half = levels_.size() / 2;
        size_t cut = static_cast<size_t>(
            std::min_element(levels_.begin() + half, levels_.end()) - levels_.begin());
        size_t cut_frame = speech_begin_ + cut;
        emit((cut_frame + 1) * frame_len_, out);
        levels_.erase(levels_.begin(), levels_.begin() + cut + 1);
        voiced_.erase(voiced_.begin(), voiced_.begin() + cut + 1);
        speech_begin_ = cut_frame + 1;
        after_cut_ = true;
        speech_frames_ = static_cast<size_t>(
            std::count(voiced_.begin(), voiced_.end(), true));
    }
}

void UtteranceSplitter::emit(size_t end, std::vector<Range>& out) {
    size_t begin = speech_begin_ * frame_len_;
    begin = begin > pad_ ? begin - pad_ : 0;
    begin = std::max(begin, emitted_end_);
    end = std::min(end, frames_ * frame_len_ + partial_.size());
    if (end > begin) out.emplace_back(begin, end);
    emitted_end_ = std::max(emitted_end_, end);
}

void LiveDictation::start(const Config& cfg, Fetch fetch, Handler handler,
                          Done done) {
    session_ = std::make_shared<Session>();
    thread_ = std::thread(&LiveDictation::run, std::move(thread_), session_, cfg,
                          std::move(fetch), std::move(handler), std::move(done));
}

void LiveDictation::finish(std::vector<float> samples) {
    if (!session_) return;
    std::lock_guard<std::mutex> lk(session_->mtx);
    session_->final_samples = std::move(samples);
    session_->finished = true;
}

void LiveDictation::flush() {
    if (!session_) return;
    std::lock_guard<std::mutex> lk(session_->mtx);
    session_->flush = true;
}

void LiveDictation::cancel() {
    if (session_) session_->cancelled.store(true);
}

void LiveDictation::join() {
    if (thread_.joinable()) thread_.join();
}

void LiveDictation::run(std::thread previous, std::shared_ptr<Session> session,
                        Config cfg, Fetch fetch, Handler handler, Done done) {
    if (previous.joinable()) previous.join();

    UtteranceSplitter splitter(cfg.sample_rate, cfg.stream_pause_ms,
                               cfg.stream_max_utterance_ms);
    std::vector<float> audio;  // the recording so far
    bool last = false;

    while (!session->cancelled.load() && !last) {
        std::vector<float> fresh;
        bool flush = false;
        {
            std::lock_guard<std::mutex> lk(session->mtx);
            flush = session->flush;
            session->flush = false;
            if (session->finished) {
                last = true;
                const auto& all = session->final_samples;
                if (all.size() > audio.size())
                    fresh.assign(all.begin() + audio.size(), all.end());
            }
        }
        if (!last) fresh = fetch(audio.size());

        audio.insert(audio.end(), fresh.begin(), fresh.end());
        auto ranges = splitter.feed(fresh.data(), fresh.size());
        if (last || flush) {
            auto rest = splitter.flush();
            ranges.insert(ranges.end(), rest.begin(), rest.end());
        }

        for (const auto& [begin, end] : ranges) {
            if (session->cancelled.load()) break;
            std::cerr << "[stream] Utterance " << begin / float(cfg.sample_rate)
                      << "s – " << end / float(cfg.sample_rate) << "s\n";
            std::vector<float> utterance(audio.begin() + begin, audio.begin() + end);
            handler(utterance, session->cancelled);
        }

        if (!last && ranges.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (done) done(session->cancelled.load());
}
