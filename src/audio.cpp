#include "audio.h"
#include <pulse/simple.h>
#include <pulse/error.h>
#include <iostream>
#include <thread>
#include <cstring>

struct AudioRecorder::PaImpl {
    pa_simple* stream = nullptr;
    std::thread thread;
};

bool AudioRecorder::init(const Config& cfg) {
    cfg_ = cfg;
    pa_ = new PaImpl;
    std::cerr << "[audio] Initialized (device="
              << (cfg.audio_device.empty() ? "default" : cfg.audio_device)
              << ", rate=" << cfg.sample_rate << "Hz)\n";
    return true;
}

void AudioRecorder::start() {
    if (recording_.load()) return;

    {
        std::lock_guard<std::mutex> lk(samples_mtx_);
        samples_.clear();
    }
    cancel_.store(false);
    recording_.store(true);

    pa_->thread = std::thread(&AudioRecorder::record_loop, this);
}

std::vector<float> AudioRecorder::stop() {
    recording_.store(false);
    if (pa_->thread.joinable())
        pa_->thread.join();

    std::lock_guard<std::mutex> lk(samples_mtx_);
    return std::move(samples_);
}

void AudioRecorder::cancel() {
    cancel_.store(true);
    recording_.store(false);
    if (pa_->thread.joinable())
        pa_->thread.join();

    std::lock_guard<std::mutex> lk(samples_mtx_);
    samples_.clear();
}

void AudioRecorder::record_loop() {
    pa_sample_spec ss{};
    ss.format   = PA_SAMPLE_FLOAT32LE;
    ss.channels = 1;
    ss.rate     = (uint32_t)cfg_.sample_rate;

    int err = 0;
    const char* dev = cfg_.audio_device.empty() ? nullptr : cfg_.audio_device.c_str();

    // Force small buffer to get frequent audio delivery (~50ms chunks)
    const size_t chunk_frames = cfg_.sample_rate / 20;  // 800 frames at 16kHz
    pa_buffer_attr ba{};
    ba.maxlength = (uint32_t)-1;                             // let server decide
    ba.fragsize  = (uint32_t)(chunk_frames * sizeof(float)); // request ~50ms fragments

    pa_->stream = pa_simple_new(
        nullptr,            // server
        "whisper-hotkey",   // app name
        PA_STREAM_RECORD,
        dev,                // device
        "recording",        // description
        &ss,
        nullptr,            // channel map
        &ba,                // buffering attrs
        &err
    );

    if (!pa_->stream) {
        std::cerr << "[audio] Failed to open PulseAudio stream: "
                  << pa_strerror(err) << "\n";
        recording_.store(false);
        return;
    }

    std::cerr << "[audio] Recording started (fragsize=" << ba.fragsize << " bytes)\n";

    // Read in chunks of ~50ms
    std::vector<float> chunk(chunk_frames);

    while (recording_.load() && !cancel_.load()) {
        if (pa_simple_read(pa_->stream, chunk.data(),
                           chunk.size() * sizeof(float), &err) < 0) {
            std::cerr << "[audio] Read error: " << pa_strerror(err) << "\n";
            break;
        }

        {
            std::lock_guard<std::mutex> lk(samples_mtx_);
            samples_.insert(samples_.end(), chunk.begin(), chunk.end());
        }

        if (chunk_cb_)
            chunk_cb_(chunk.data(), chunk.size());
    }

    // Drain remaining audio from PulseAudio's internal buffer
    if (!cancel_.load()) {
        pa_usec_t latency = pa_simple_get_latency(pa_->stream, &err);
        if (latency > 0) {
            size_t remaining_frames = (size_t)((latency * cfg_.sample_rate) / 1000000);
            std::cerr << "[audio] Draining " << remaining_frames << " buffered frames ("
                      << latency / 1000 << "ms)\n";
            while (remaining_frames > 0) {
                size_t to_read = std::min(chunk.size(), remaining_frames);
                if (pa_simple_read(pa_->stream, chunk.data(),
                                   to_read * sizeof(float), &err) < 0) {
                    break;
                }
                {
                    std::lock_guard<std::mutex> lk(samples_mtx_);
                    samples_.insert(samples_.end(), chunk.begin(), chunk.begin() + to_read);
                }
                if (chunk_cb_)
                    chunk_cb_(chunk.data(), to_read);
                remaining_frames -= to_read;
            }
        }
    }

    pa_simple_free(pa_->stream);
    pa_->stream = nullptr;

    std::cerr << "[audio] Recording stopped ("
              << samples_.size() / (float)cfg_.sample_rate << "s)\n";
}

void AudioRecorder::shutdown() {
    if (recording_.load()) cancel();
    delete pa_;
    pa_ = nullptr;
}

AudioRecorder::~AudioRecorder() {
    if (pa_) shutdown();
}
