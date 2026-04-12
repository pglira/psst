#include "config.h"
#include <toml++/toml.hpp>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

std::string default_config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    fs::path base = xdg ? fs::path(xdg) : fs::path(std::getenv("HOME")) / ".config";
    return (base / "psst" / "config.toml").string();
}

Config load_config(const std::string& path) {
    Config cfg;

    if (!fs::exists(path)) {
        std::cerr << "[config] File not found: " << path
                  << " — using defaults\n";
        return cfg;
    }

    try {
        auto tbl = toml::parse_file(path);

        // whisper
        cfg.model_path  = tbl["whisper"]["model_path"].value_or(cfg.model_path);
        cfg.model_size  = tbl["whisper"]["model_size"].value_or(cfg.model_size);
        cfg.language    = tbl["whisper"]["language"].value_or(cfg.language);
        cfg.translate   = tbl["whisper"]["translate"].value_or(cfg.translate);
        cfg.threads     = tbl["whisper"]["threads"].value_or(cfg.threads);

        // gpu
        cfg.gpu_enabled = tbl["gpu"]["enabled"].value_or(cfg.gpu_enabled);
        cfg.gpu_device  = tbl["gpu"]["device"].value_or(cfg.gpu_device);

        // hotkey
        cfg.hotkey_bind = tbl["hotkey"]["bind"].value_or(cfg.hotkey_bind);

        // audio
        cfg.audio_device = tbl["audio"]["device"].value_or(cfg.audio_device);
        cfg.sample_rate  = tbl["audio"]["sample_rate"].value_or(cfg.sample_rate);

        // tray
        cfg.tray_enabled = tbl["tray"]["enabled"].value_or(cfg.tray_enabled);

        std::cerr << "[config] Loaded from " << path << "\n";
    } catch (const toml::parse_error& err) {
        std::cerr << "[config] Parse error in " << path << ": " << err << "\n";
    }

    return cfg;
}
