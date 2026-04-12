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
    // Thread-safe: only one transcription runs at a time.
    std::string transcribe(const std::vector<float>& pcm);

    // Release model resources.
    void shutdown();

    bool is_ready() const { return ctx_ != nullptr; }
    bool is_busy() const { return busy_.load(); }

    ~Transcriber();

private:
    whisper_context* ctx_ = nullptr;
    Config cfg_;
    std::mutex mtx_;
    std::atomic<bool> busy_{false};
};
