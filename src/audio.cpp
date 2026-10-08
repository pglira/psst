#include "audio.h"
#include <pulse/simple.h>
#include <pulse/error.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstring>

struct AudioRecorder::Session {
    std::atomic<bool> running{true};
    std::atomic<bool> open{false};

    std::mutex mtx;
    std::vector<float> samples;  // guarded by mtx
    bool capturing = false;      // guarded by mtx
};

// Recording threads that have not ended yet.
static std::atomic<int> g_threads{0};

bool AudioRecorder::init(const Config& cfg) {
    cfg_ = cfg;
    std::cerr << "[audio] Initialized (device="
              << (cfg.audio_device.empty() ? "default" : cfg.audio_device)
              << ", rate=" << cfg.sample_rate << "Hz)\n";
    return true;
}

void AudioRecorder::start() {
    if (session_) return;
    session_ = std::make_shared<Session>();
    ++g_threads;
    std::thread(&AudioRecorder::record_loop, cfg_, session_, chunk_cb_, error_cb_).detach();
}

void AudioRecorder::stop() {
    if (!session_) return;
    {
        std::lock_guard<std::mutex> lk(session_->mtx);
        session_->running.store(false);
        session_->samples.clear();
        session_->capturing = false;
    }
    session_.reset();
}

bool AudioRecorder::mic_open() const {
    return session_ && session_->open.load();
}

void AudioRecorder::begin_capture() {
    if (!session_) return;
    std::lock_guard<std::mutex> lk(session_->mtx);
    session_->capturing = true;
}

std::vector<float> AudioRecorder::end_capture() {
    if (!session_) return {};
    std::lock_guard<std::mutex> lk(session_->mtx);
    session_->capturing = false;
    std::vector<float> out = std::move(session_->samples);
    session_->samples.clear();
    return out;
}

void AudioRecorder::record_loop(Config cfg, std::shared_ptr<Session> s,
                                ChunkCallback chunk_cb, ErrorCallback error_cb) {
    pa_sample_spec ss{};
    ss.format   = PA_SAMPLE_FLOAT32LE;
    ss.channels = 1;
    ss.rate     = (uint32_t)cfg.sample_rate;

    int err = 0;
    const char* dev = cfg.audio_device.empty() ? nullptr : cfg.audio_device.c_str();

    // Force small buffer to get frequent audio delivery (~50ms chunks)
    const size_t chunk_frames = cfg.sample_rate / 20;  // 800 frames at 16kHz
    pa_buffer_attr ba{};
    ba.maxlength = (uint32_t)-1;                             // let server decide
    ba.fragsize  = (uint32_t)(chunk_frames * sizeof(float)); // request ~50ms fragments

    // Blocks until the audio server answers or times out.
    pa_simple* stream = pa_simple_new(
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

    if (!stream) {
        std::string msg = std::string("The microphone could not be opened: ") + pa_strerror(err);
        std::cerr << "[audio] Failed to open PulseAudio stream: " << pa_strerror(err) << "\n";
        if (s->running.load() && error_cb) error_cb(msg);
        --g_threads;
        return;
    }

    s->open.store(true);
    std::cerr << "[audio] Microphone open (fragsize=" << ba.fragsize << " bytes)\n";

    // Read in chunks of ~50ms
    std::vector<float> chunk(chunk_frames);

    const size_t preroll = (size_t)cfg.sample_rate * kPrerollMs / 1000;
    while (s->running.load()) {
        if (pa_simple_read(stream, chunk.data(),
                           chunk.size() * sizeof(float), &err) < 0) {
            std::cerr << "[audio] Read error: " << pa_strerror(err) << "\n";
            if (s->running.load() && error_cb)
                error_cb(std::string("The microphone stopped: ") + pa_strerror(err));
            break;
        }

        {
            std::lock_guard<std::mutex> lk(s->mtx);
            if (!s->running.load()) break;
            s->samples.insert(s->samples.end(), chunk.begin(), chunk.end());
            if (!s->capturing && s->samples.size() > preroll)
                s->samples.erase(s->samples.begin(), s->samples.end() - (long)preroll);
        }

        if (chunk_cb)
            chunk_cb(chunk.data(), chunk.size());
    }

    s->open.store(false);
    pa_simple_free(stream);

    std::cerr << "[audio] Microphone closed\n";
    --g_threads;
}

void AudioRecorder::shutdown() {
    stop();
    // Give open streams time to close; a thread that still waits for the
    // audio server is left behind.
    for (int i = 0; i < 20 && g_threads.load() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

AudioRecorder::~AudioRecorder() {
    stop();
}
