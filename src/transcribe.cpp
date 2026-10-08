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

bool Transcriber::reload(const Config& cfg) {
    std::lock_guard<std::mutex> load_lock(load_mtx_);
    if (shut_down_) return false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        bool same_model = ctx_ && cfg.model_size == cfg_.model_size &&
                          cfg.model_path == cfg_.model_path &&
                          cfg.gpu_enabled == cfg_.gpu_enabled &&
                          cfg.gpu_device == cfg_.gpu_device;
        if (same_model) {
            cfg_ = cfg;
            return true;
        }
    }

    whisper_context* ctx = load_model(cfg);
    if (!ctx) return false;

    whisper_context* old = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        old = ctx_;
        ctx_ = ctx;
        cfg_ = cfg;
    }
    if (old) whisper_free(old);
    return true;
}

bool Transcriber::init(const Config& cfg) {
    cfg_ = cfg;
    ctx_ = load_model(cfg);
    return ctx_ != nullptr;
}

whisper_context* Transcriber::load_model(const Config& cfg) {
    std::string path = resolve_model_path(cfg);
    if (path.empty()) return nullptr;

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu    = cfg.gpu_enabled;
    cparams.gpu_device = cfg.gpu_device;
    cparams.flash_attn = false;  // flash attention can cause 0 segments on some CUDA versions

    std::cerr << "[whisper] Loading model: " << path
              << " (GPU=" << (cfg.gpu_enabled ? "yes" : "no")
              << ", device=" << cfg.gpu_device << ")\n";

    whisper_context* ctx = whisper_init_from_file_with_params(path.c_str(), cparams);
    if (!ctx) {
        std::cerr << "[whisper] Failed to load model\n";
        return nullptr;
    }

    std::cerr << "[whisper] Model loaded successfully\n";
    return ctx;
}

std::string Transcriber::transcribe(const std::vector<float>& pcm_raw,
                                    const std::string& context) {
    if (pcm_raw.empty()) return {};

    std::vector<float> pcm = pcm_raw;

    // 1. Remove DC offset
    float dc = 0.0f;
    for (size_t i = 0; i < pcm.size(); ++i)
        dc += pcm[i];
    dc /= (float)pcm.size();
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] -= dc;

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
    }

    // 3. Pad short audio with silence: whisper rejects input under 1 s.
    const size_t min_samples = 16000 * 11 / 10;
    if (pcm.size() < min_samples)
        pcm.resize(min_samples, 0.0f);

    std::lock_guard<std::mutex> lock(mtx_);
    if (!ctx_) return {};

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    params.print_progress   = false;
    params.print_special    = false;
    params.print_realtime   = false;
    params.print_timestamps = false;

    params.language = cfg_.language == "auto" ? "auto" : cfg_.language.c_str();
    params.translate = cfg_.translate;
    params.suppress_nst = true;
    std::string prompt = cfg_.initial_prompt;
    if (!context.empty()) {
        // Keep the end of the context; whisper uses at most ~220 prompt tokens.
        size_t from = context.size() > 400 ? context.size() - 400 : 0;
        while (from < context.size() &&
               (static_cast<unsigned char>(context[from]) & 0xC0) == 0x80)
            ++from;
        if (!prompt.empty()) prompt += ' ';
        prompt += context.substr(from);
    }
    if (!prompt.empty())
        params.initial_prompt = prompt.c_str();

    if (cfg_.threads > 0)
        params.n_threads = cfg_.threads;

    std::cerr << "[whisper] Transcribing " << pcm_raw.size() / 16000.0f
              << "s of audio...\n";

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
    busy_.store(false);

    // Trim leading/trailing whitespace
    auto start = result.find_first_not_of(" \t\n\r");
    auto end   = result.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return {};
    result = result.substr(start, end - start + 1);

    std::cerr << "[whisper] Result: \"" << result << "\"\n";
    return result;
}

void Transcriber::shutdown() {
    std::lock_guard<std::mutex> load_lock(load_mtx_);
    std::lock_guard<std::mutex> lock(mtx_);
    shut_down_ = true;
    if (ctx_) {
        whisper_free(ctx_);
        ctx_ = nullptr;
        std::cerr << "[whisper] Model unloaded\n";
    }
}

Transcriber::~Transcriber() {
    shutdown();
}
