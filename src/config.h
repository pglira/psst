#pragma once
#include <string>
#include <cstdint>

struct Config {
    // whisper
    std::string model_path;
    std::string model_size = "small";
    std::string language   = "auto";
    bool        translate  = false;
    int         threads    = 0;

    // gpu
    bool gpu_enabled = true;
    int  gpu_device  = 0;

    // hotkey
    std::string hotkey_bind = "super+v";

    // audio
    std::string audio_device;
    int         sample_rate = 16000;

    // overlay
    int   overlay_width   = 400;
    int   overlay_height  = 150;
    float overlay_opacity = 0.85f;
    int   overlay_bars    = 32;

    // tray
    bool tray_enabled = true;
};

// Load config from TOML file. Missing fields use defaults.
Config load_config(const std::string& path);

// Return XDG config path: ~/.config/whisper-hotkey/config.toml
std::string default_config_path();
