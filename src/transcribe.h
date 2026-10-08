#pragma once
#include "config.h"
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

struct whisper_context;

class Transcriber {
public:
    // Load model from file (or auto-download based on config).
    // Call once at startup — the model stays in memory.
    bool init(const Config& cfg);

    // Transcribe PCM audio (float32, 16kHz, mono).
    // Returns the transcribed text (all segments concatenated).
    // `context` is text that precedes the audio; it conditions the style and
    // spelling of the result.
    // Thread-safe: only one transcription runs at a time.
    std::string transcribe(const std::vector<float>& pcm,
                           const std::string& context = {});

    // Use the settings of `cfg` for the next transcriptions. If `cfg` selects
    // another model, load it first; transcriptions continue with the old
    // model while it loads. Loads run one at a time. Returns false if the
    // load fails; the old model and settings then stay.
    bool reload(const Config& cfg);

    // Release model resources.
    void shutdown();

    bool is_ready() const { return ctx_ != nullptr; }
    bool is_busy() const { return busy_.load(); }

    ~Transcriber();

private:
    whisper_context* ctx_ = nullptr;
    Config cfg_;
    static whisper_context* load_model(const Config& cfg);

    std::mutex mtx_;        // guards ctx_ and cfg_
    std::mutex load_mtx_;   // serializes model loads and shutdown
    bool shut_down_ = false;
    std::atomic<bool> busy_{false};
};
