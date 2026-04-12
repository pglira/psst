#include "transcribe.h"
#include <whisper.h>
#include <iostream>
#include <filesystem>
#include <cstdlib>
#include <cmath>
#include <sstream>

namespace fs = std::filesystem;

static std::string resolve_model_path(const Config& cfg) {
    if (!cfg.model_path.empty() && fs::exists(cfg.model_path))
        return cfg.model_path;

    // Try XDG data dir
    const char* xdg = std::getenv("XDG_DATA_HOME");
    fs::path base = xdg ? fs::path(xdg)
                        : fs::path(std::getenv("HOME")) / ".local" / "share";
    fs::path dir = base / "psst" / "models";
    std::string filename = "ggml-" + cfg.model_size + ".bin";
    fs::path model = dir / filename;

    if (fs::exists(model))
        return model.string();

    // Auto-download
    std::cerr << "[whisper] Model not found, downloading " << cfg.model_size
              << " to " << dir.string() << " ...\n";
    fs::create_directories(dir);

    std::string url =
        "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/" + filename;
    std::ostringstream cmd;
    cmd << "curl -L --progress-bar -o '"
        << model.string() << "' '" << url << "'";

    int ret = std::system(cmd.str().c_str());
    if (ret != 0 || !fs::exists(model)) {
        std::cerr << "[whisper] Download failed. Please manually download:\n"
                  << "  " << url << "\n"
                  << "  and place it at: " << model.string() << "\n";
        return {};
    }

    std::cerr << "[whisper] Downloaded model to " << model.string() << "\n";
    return model.string();
}

bool Transcriber::init(const Config& cfg) {
    cfg_ = cfg;
    std::string path = resolve_model_path(cfg);
    if (path.empty()) return false;

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu    = cfg.gpu_enabled;
    cparams.gpu_device = cfg.gpu_device;
    cparams.flash_attn = false;  // flash attention can cause 0 segments on some CUDA versions

    std::cerr << "[whisper] Loading model: " << path
              << " (GPU=" << (cfg.gpu_enabled ? "yes" : "no")
              << ", device=" << cfg.gpu_device << ")\n";

    ctx_ = whisper_init_from_file_with_params(path.c_str(), cparams);
    if (!ctx_) {
        std::cerr << "[whisper] Failed to load model\n";
        return false;
    }

    std::cerr << "[whisper] Model loaded successfully\n";
    return true;
}

std::string Transcriber::transcribe(const std::vector<float>& pcm_raw) {
    if (!ctx_ || pcm_raw.empty()) return {};

    std::vector<float> pcm = pcm_raw;

    // Save raw audio for debugging
    {
        const char* dbg = "/tmp/psst-raw.wav";
        FILE* f = fopen(dbg, "wb");
        if (f) {
            int16_t bps = 16;
            int32_t sr = 16000, byteRate = sr * 2, dataSize = (int32_t)(pcm.size() * 2);
            int16_t blockAlign = 2, channels = 1;
            int32_t chunkSize = 36 + dataSize;
            fwrite("RIFF", 1, 4, f); fwrite(&chunkSize, 4, 1, f);
            fwrite("WAVEfmt ", 1, 8, f);
            int32_t sc1 = 16; int16_t fmt = 1;
            fwrite(&sc1, 4, 1, f); fwrite(&fmt, 2, 1, f);
            fwrite(&channels, 2, 1, f); fwrite(&sr, 4, 1, f);
            fwrite(&byteRate, 4, 1, f); fwrite(&blockAlign, 2, 1, f);
            fwrite(&bps, 2, 1, f);
            fwrite("data", 1, 4, f); fwrite(&dataSize, 4, 1, f);
            for (size_t i = 0; i < pcm.size(); ++i) {
                float clamped = std::max(-1.0f, std::min(1.0f, pcm[i]));
                int16_t s = (int16_t)(clamped * 32767.0f);
                fwrite(&s, 2, 1, f);
            }
            fclose(f);
            std::cerr << "[whisper] Raw WAV saved to " << dbg << "\n";
        }
    }

    // 1. Remove DC offset
    float dc = 0.0f;
    for (size_t i = 0; i < pcm.size(); ++i)
        dc += pcm[i];
    dc /= (float)pcm.size();
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] -= dc;
    std::cerr << "[whisper] DC offset removed: " << dc << "\n";

    // 2. Normalize to [-1, 1] range
    float peak = 0.0f;
    for (size_t i = 0; i < pcm.size(); ++i) {
        float a = fabsf(pcm[i]);
        if (a > peak) peak = a;
    }
    if (peak > 0.0001f) {
        // Normalize to ~0.9 peak to leave headroom
        float scale = 0.9f / peak;
        for (size_t i = 0; i < pcm.size(); ++i)
            pcm[i] *= scale;
        std::cerr << "[whisper] Normalized audio: peak=" << peak
                  << " scale=" << scale << "\n";
    }

    // Audio diagnostics (post-normalization)
    float mn = 0, mx = 0, rms = 0;
    for (size_t i = 0; i < pcm.size(); ++i) {
        float s = pcm[i];
        if (s < mn) mn = s;
        if (s > mx) mx = s;
        rms += s * s;
    }
    rms = sqrtf(rms / (float)pcm.size());
    std::cerr << "[whisper] Audio stats: min=" << mn << " max=" << mx
              << " rms=" << rms << " samples=" << pcm.size() << "\n";

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    params.print_progress   = false;
    params.print_special    = false;
    params.print_realtime   = false;
    params.print_timestamps = false;

    params.language = cfg_.language == "auto" ? "auto" : cfg_.language.c_str();
    params.translate = cfg_.translate;

    if (cfg_.threads > 0)
        params.n_threads = cfg_.threads;

    std::cerr << "[whisper] Transcribing " << pcm.size() / 16000.0f
              << "s of audio...\n";

    // Debug: save WAV for inspection
    {
        const char* dbg = "/tmp/psst-debug.wav";
        FILE* f = fopen(dbg, "wb");
        if (f) {
            int16_t bps = 16;
            int32_t sr = 16000, byteRate = sr * 2, dataSize = (int32_t)(pcm.size() * 2);
            int16_t blockAlign = 2, channels = 1;
            int32_t chunkSize = 36 + dataSize;
            fwrite("RIFF", 1, 4, f); fwrite(&chunkSize, 4, 1, f);
            fwrite("WAVEfmt ", 1, 8, f);
            int32_t sc1 = 16; int16_t fmt = 1;
            fwrite(&sc1, 4, 1, f); fwrite(&fmt, 2, 1, f);
            fwrite(&channels, 2, 1, f); fwrite(&sr, 4, 1, f);
            fwrite(&byteRate, 4, 1, f); fwrite(&blockAlign, 2, 1, f);
            fwrite(&bps, 2, 1, f);
            fwrite("data", 1, 4, f); fwrite(&dataSize, 4, 1, f);
            for (size_t i = 0; i < pcm.size(); ++i) {
                float clamped = std::max(-1.0f, std::min(1.0f, pcm[i]));
                int16_t s = (int16_t)(clamped * 32767.0f);
                fwrite(&s, 2, 1, f);
            }
            fclose(f);
            std::cerr << "[whisper] Debug WAV saved to " << dbg << "\n";
        }
    }

    std::lock_guard<std::mutex> lock(mtx_);
    busy_.store(true);

    // Create a fresh state for each transcription to avoid stale GPU state
    struct whisper_state* state = whisper_init_state(ctx_);
    if (!state) {
        std::cerr << "[whisper] Failed to create state\n";
        busy_.store(false);
        return {};
    }

    int ret = whisper_full_with_state(ctx_, state, params, pcm.data(), (int)pcm.size());
    if (ret != 0) {
        std::cerr << "[whisper] Inference failed (code " << ret << ")\n";
        whisper_free_state(state);
        busy_.store(false);
        return {};
    }

    std::string result;
    int n = whisper_full_n_segments_from_state(state);
    std::cerr << "[whisper] Got " << n << " segment(s)\n";
    for (int i = 0; i < n; ++i) {
        const char* text = whisper_full_get_segment_text_from_state(state, i);
        if (text) result += text;
    }

    whisper_free_state(state);

    // Trim leading/trailing whitespace
    auto start = result.find_first_not_of(" \t\n\r");
    auto end   = result.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return {};
    result = result.substr(start, end - start + 1);

    std::cerr << "[whisper] Result: \"" << result << "\"\n";
    busy_.store(false);
    return result;
}

void Transcriber::shutdown() {
    if (ctx_) {
        whisper_free(ctx_);
        ctx_ = nullptr;
        std::cerr << "[whisper] Model unloaded\n";
    }
}

Transcriber::~Transcriber() {
    shutdown();
}
