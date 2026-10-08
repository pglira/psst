#pragma once
#include <string>
#include <cstdint>
#include <map>

struct Config {
    // whisper
    std::string model_path;
    std::string model_size = "small";
    std::string language   = "auto";
    bool        translate  = false;
    int         threads    = 0;
    std::string initial_prompt;

    // punctuation (spoken commands → symbols)
    bool punctuation_enabled = true;
    std::map<std::string, std::string> punctuation_words;  // phrase → text

    // stream (type text while speaking)
    bool stream_enabled          = true;
    int  stream_pause_ms         = 600;    // silence that ends an utterance
    int  stream_max_utterance_ms = 20000;  // longer utterances are split

    // editor (dictation window with an editable text field)
    bool        editor_enabled   = true;
    std::string editor_pause_key = "<Control>space";
    std::string editor_type_key  = "<Control>Return";
    std::string editor_copy_key  = "<Control><Shift>Return";

    // gpu
    bool gpu_enabled = true;
    int  gpu_device  = 0;

    // hotkey
    std::string hotkey_bind = "super+v";

    // output
    bool copy_to_clipboard = false;

    // inject
    int type_delay_ms = 12;

    // audio
    std::string audio_device;
    int         sample_rate = 16000;
};

// Load config from TOML file. Missing fields use defaults.
Config load_config(const std::string& path);

// Write the settings of `cfg` to the TOML file at `path`. Lines with other
// keys and comments stay as they are; missing keys and sections are added.
// [punctuation.words] is not written. Returns false on a write error.
bool save_config(const std::string& path, const Config& cfg);

// Return XDG config path: ~/.config/psst/config.toml
std::string default_config_path();
