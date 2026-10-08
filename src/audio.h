#pragma once
#include "config.h"
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

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

    // Close the microphone and drop all input. Never blocks: a background
    // thread that still waits for the audio server ends on its own later.
    void stop();

    // True if the microphone is open and delivers input.
    bool mic_open() const;

    // Keep all input from now on, including the pre-roll.
    void begin_capture();

    // Stop the capture and return its samples (f32, 16 kHz, mono).
    std::vector<float> end_capture();

    // Callback invoked from the recording thread with each chunk of new samples.
    // Used by the editor to display the VU meter.
    using ChunkCallback = std::function<void(const float* data, size_t count)>;
    void set_chunk_callback(ChunkCallback cb) { chunk_cb_ = std::move(cb); }

    // Callback invoked from the recording thread when the microphone does
    // not open or stops with an error.
    using ErrorCallback = std::function<void(const std::string& message)>;
    void set_error_callback(ErrorCallback cb) { error_cb_ = std::move(cb); }

    void shutdown();
    ~AudioRecorder();

private:
    // State of one opening of the microphone, shared with its thread.
    struct Session;
    static void record_loop(Config cfg, std::shared_ptr<Session> s,
                            ChunkCallback chunk_cb, ErrorCallback error_cb);

    Config cfg_;
    std::shared_ptr<Session> session_;  // null while the microphone is closed

    ChunkCallback chunk_cb_;
    ErrorCallback error_cb_;
};
