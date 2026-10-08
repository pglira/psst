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

    // editor (dictation window with an editable text field)
    std::string editor_talk_key  = "<Control>space";  // hold to dictate
    std::string editor_type_key  = "<Control>Return";
    std::string editor_copy_key  = "<Control><Shift>Return";
    std::string editor_correct_key = "<Control>r";
    std::string editor_undo_key    = "<Control>z";

    // correction (LLM correction of the editor text with the claude CLI)
    std::string correction_command = "claude";
    std::string correction_model   = "haiku";
    std::string correction_prompt  =
        "You correct text from speech recognition. Fix recognition errors, "
        "spelling, grammar and punctuation. Speech recognition often writes a "
        "word that sounds like the intended word but does not fit the context, "
        "especially technical terms, product names and names (for example "
        "\"cloud\" for \"Claude\", \"def container\" for \"devcontainer\", "
        "\"get hub\" for \"GitHub\"). Replace such words with the word that the "
        "context implies. Keep the wording, the language, the meaning and the "
        "line breaks. Do not add, remove or translate content. Output only the "
        "corrected text.";

    // gpu
    bool gpu_enabled = true;
    int  gpu_device  = 0;

    // hotkey
    std::string hotkey_bind = "super+v";

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
