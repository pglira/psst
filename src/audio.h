#pragma once
#include "config.h"
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>

class AudioRecorder {
public:
    bool init(const Config& cfg);

    // Start recording in a background thread.
    void start();

    // Stop recording. Returns the accumulated PCM samples (f32, 16kHz, mono).
    std::vector<float> stop();

    // Discard current recording without returning samples.
    void cancel();

    bool is_recording() const { return recording_.load(); }

    // Callback invoked from the recording thread with each chunk of new samples.
    // Used by the overlay to display the spectrum.
    using ChunkCallback = std::function<void(const float* data, size_t count)>;
    void set_chunk_callback(ChunkCallback cb) { chunk_cb_ = std::move(cb); }

    void shutdown();
    ~AudioRecorder();

private:
    void record_loop();

    Config cfg_;
    std::atomic<bool> recording_{false};
    std::atomic<bool> cancel_{false};

    std::mutex samples_mtx_;
    std::vector<float> samples_;

    ChunkCallback chunk_cb_;

    struct PaImpl;
    PaImpl* pa_ = nullptr;
};
