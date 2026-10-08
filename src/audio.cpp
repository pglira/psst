#include "audio.h"
#include <pulse/simple.h>
#include <pulse/error.h>
#include <iostream>
#include <thread>
#include <algorithm>
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
    if (running_.load()) return;
    {
        std::lock_guard<std::mutex> lk(samples_mtx_);
        samples_.clear();
        capturing_ = false;
    }
    open_failed_.store(false);
    running_.store(true);
    pa_->thread = std::thread(&AudioRecorder::record_loop, this);
}

void AudioRecorder::stop() {
    running_.store(false);
    if (pa_ && pa_->thread.joinable())
        pa_->thread.join();
    std::lock_guard<std::mutex> lk(samples_mtx_);
    samples_.clear();
    capturing_ = false;
}

void AudioRecorder::begin_capture() {
    std::lock_guard<std::mutex> lk(samples_mtx_);
    capturing_ = true;
}

std::vector<float> AudioRecorder::end_capture() {
    std::lock_guard<std::mutex> lk(samples_mtx_);
    capturing_ = false;
    std::vector<float> out = std::move(samples_);
    samples_.clear();
    return out;
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
        "psst",   // app name
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
        open_failed_.store(true);
        running_.store(false);
        return;
    }

    std::cerr << "[audio] Microphone open (fragsize=" << ba.fragsize << " bytes)\n";

    // Read in chunks of ~50ms
    std::vector<float> chunk(chunk_frames);

    const size_t preroll = (size_t)cfg_.sample_rate * kPrerollMs / 1000;
    while (running_.load()) {
        if (pa_simple_read(pa_->stream, chunk.data(),
                           chunk.size() * sizeof(float), &err) < 0) {
            std::cerr << "[audio] Read error: " << pa_strerror(err) << "\n";
            break;
        }

        {
            std::lock_guard<std::mutex> lk(samples_mtx_);
            samples_.insert(samples_.end(), chunk.begin(), chunk.end());
            if (!capturing_ && samples_.size() > preroll)
                samples_.erase(samples_.begin(), samples_.end() - (long)preroll);
        }

        if (chunk_cb_)
            chunk_cb_(chunk.data(), chunk.size());
    }

    pa_simple_free(pa_->stream);
    pa_->stream = nullptr;

    std::cerr << "[audio] Microphone closed\n";
}

void AudioRecorder::shutdown() {
    stop();
    delete pa_;
    pa_ = nullptr;
}

AudioRecorder::~AudioRecorder() {
    if (pa_) shutdown();
}
