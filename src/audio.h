#pragma once
#include "config.h"
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>

// Microphone input for push-to-talk.
//
// While the microphone is open, the recorder keeps only the last
// kPrerollMs of input, so that a capture also contains the start of a word
// that was spoken right before begin_capture().
class AudioRecorder {
public:
    static constexpr int kPrerollMs = 300;

    bool init(const Config& cfg);

    // Use the audio settings of `cfg` when the microphone opens next. Call
    // only while the microphone is closed.
    void configure(const Config& cfg) { cfg_ = cfg; }

    // Open the microphone in a background thread.
    void start();

    // Close the microphone and drop all input.
    void stop();

    // True if the microphone did not open at the last start().
    bool open_failed() const { return open_failed_.load(); }

    // Keep all input from now on, including the pre-roll.
    void begin_capture();

    // Stop the capture and return its samples (f32, 16 kHz, mono).
    std::vector<float> end_capture();

    // Callback invoked from the recording thread with each chunk of new samples.
    // Used by the editor to display the VU meter.
    using ChunkCallback = std::function<void(const float* data, size_t count)>;
    void set_chunk_callback(ChunkCallback cb) { chunk_cb_ = std::move(cb); }

    void shutdown();
    ~AudioRecorder();

private:
    void record_loop();

    Config cfg_;
    std::atomic<bool> running_{false};
    std::atomic<bool> open_failed_{false};

    std::mutex samples_mtx_;
    std::vector<float> samples_;
    bool capturing_ = false;  // guarded by samples_mtx_

    ChunkCallback chunk_cb_;

    struct PaImpl;
    PaImpl* pa_ = nullptr;
};
