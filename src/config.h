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

    // output
    bool copy_to_clipboard = false;

    // audio
    std::string audio_device;
    int         sample_rate = 16000;

    // sound
    bool sound_enabled = true;
};

// Load config from TOML file. Missing fields use defaults.
Config load_config(const std::string& path);

// Return XDG config path: ~/.config/psst/config.toml
std::string default_config_path();
