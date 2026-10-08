#include "config.h"
#include <toml++/toml.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

std::string default_config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    fs::path base = xdg ? fs::path(xdg) : fs::path(std::getenv("HOME")) / ".config";
    return (base / "psst" / "config.toml").string();
}

namespace {

std::string toml_string(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:   out += c;
        }
    }
    return out + "\"";
}

std::string toml_bool(bool b) { return b ? "true" : "false"; }

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t");
    auto end   = s.find_last_not_of(" \t\r");
    return start == std::string::npos ? std::string() : s.substr(start, end - start + 1);
}

// Section name of a "[name]" line, or empty for other lines.
std::string section_of(const std::string& line) {
    std::string t = trim(line);
    if (t.size() < 3 || t[0] != '[' || t[1] == '[') return {};
    auto close = t.find(']');
    return close == std::string::npos ? std::string() : trim(t.substr(1, close - 1));
}

// Key of a "key = value" line, or empty for other lines.
std::string key_of(const std::string& line) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == '[') return {};
    auto eq = t.find('=');
    return eq == std::string::npos ? std::string() : trim(t.substr(0, eq));
}

// The part of a "key = value  # comment" line after its value.
std::string trailing_comment(const std::string& line) {
    auto eq = line.find('=');
    size_t i = line.find_first_not_of(" \t", eq + 1);
    if (i == std::string::npos) return {};
    if (line[i] == '"') {
        for (++i; i < line.size() && line[i] != '"'; ++i)
            if (line[i] == '\\') ++i;
        ++i;
    } else {
        while (i < line.size() && line[i] != '#' && line[i] != ' ' && line[i] != '\t') ++i;
    }
    auto hash = line.find('#', std::min(i, line.size()));
    if (hash == std::string::npos) return {};
    auto ws = line.find_last_not_of(" \t", hash - 1);
    return line.substr(ws == std::string::npos ? hash : ws + 1);
}

} // namespace

bool save_config(const std::string& path, const Config& cfg) {
    struct Entry { std::string section, key, value; };
    const std::vector<Entry> entries = {
        {"whisper",     "model_size",       toml_string(cfg.model_size)},
        {"whisper",     "language",         toml_string(cfg.language)},
        {"whisper",     "translate",        toml_bool(cfg.translate)},
        {"whisper",     "initial_prompt",   toml_string(cfg.initial_prompt)},
        {"punctuation", "enabled",          toml_bool(cfg.punctuation_enabled)},
        {"editor",      "talk_key",         toml_string(cfg.editor_talk_key)},
        {"editor",      "type_key",         toml_string(cfg.editor_type_key)},
        {"editor",      "copy_key",         toml_string(cfg.editor_copy_key)},
        {"editor",      "correct_key",      toml_string(cfg.editor_correct_key)},
        {"editor",      "undo_key",         toml_string(cfg.editor_undo_key)},
        {"correction",  "command",          toml_string(cfg.correction_command)},
        {"correction",  "model",            toml_string(cfg.correction_model)},
        {"correction",  "prompt",           toml_string(cfg.correction_prompt)},
        {"gpu",         "enabled",          toml_bool(cfg.gpu_enabled)},
        {"hotkey",      "bind",             toml_string(cfg.hotkey_bind)},
        {"inject",      "type_delay_ms",    std::to_string(cfg.type_delay_ms)},
        {"audio",       "device",           toml_string(cfg.audio_device)},
    };

    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        for (std::string line; std::getline(in, line);) lines.push_back(line);
    }

    for (const auto& e : entries) {
        std::string section;
        long section_line = -1, last_key_line = -1;
        bool done = false;
        for (size_t i = 0; i < lines.size() && !done; ++i) {
            std::string s = section_of(lines[i]);
            if (!s.empty()) {
                section = s;
                if (s == e.section) section_line = last_key_line = (long)i;
                continue;
            }
            if (section != e.section) continue;
            std::string key = key_of(lines[i]);
            if (key.empty()) continue;
            last_key_line = (long)i;
            if (key == e.key) {
                lines[i] = e.key + " = " + e.value + trailing_comment(lines[i]);
                done = true;
            }
        }
        if (done) continue;
        std::string line = e.key + " = " + e.value;
        if (section_line >= 0) {
            lines.insert(lines.begin() + last_key_line + 1, line);
        } else {
            if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
            lines.push_back("[" + e.section + "]");
            lines.push_back(line);
        }
    }

    fs::path dir = fs::path(path).parent_path();
    std::error_code ec;
    if (!dir.empty()) fs::create_directories(dir, ec);
    std::ofstream out(path, std::ios::trunc);  // writes through a symlink
    for (const auto& line : lines) out << line << "\n";
    out.close();
    if (!out) {
        std::cerr << "[config] Failed to write " << path << "\n";
        return false;
    }
    std::cerr << "[config] Saved to " << path << "\n";
    return true;
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
        cfg.initial_prompt = tbl["whisper"]["initial_prompt"].value_or(cfg.initial_prompt);

        // punctuation
        cfg.punctuation_enabled = tbl["punctuation"]["enabled"].value_or(cfg.punctuation_enabled);
        if (auto words = tbl["punctuation"]["words"].as_table()) {
            for (const auto& [phrase, value] : *words) {
                if (auto text = value.value<std::string>())
                    cfg.punctuation_words[std::string(phrase.str())] = *text;
            }
        }

        // editor
        cfg.editor_talk_key  = tbl["editor"]["talk_key"].value_or(cfg.editor_talk_key);
        cfg.editor_type_key  = tbl["editor"]["type_key"].value_or(cfg.editor_type_key);
        cfg.editor_copy_key  = tbl["editor"]["copy_key"].value_or(cfg.editor_copy_key);
        cfg.editor_correct_key = tbl["editor"]["correct_key"].value_or(cfg.editor_correct_key);
        cfg.editor_undo_key    = tbl["editor"]["undo_key"].value_or(cfg.editor_undo_key);

        // correction
        cfg.correction_command = tbl["correction"]["command"].value_or(cfg.correction_command);
        cfg.correction_model   = tbl["correction"]["model"].value_or(cfg.correction_model);
        cfg.correction_prompt  = tbl["correction"]["prompt"].value_or(cfg.correction_prompt);

        // gpu
        cfg.gpu_enabled = tbl["gpu"]["enabled"].value_or(cfg.gpu_enabled);
        cfg.gpu_device  = tbl["gpu"]["device"].value_or(cfg.gpu_device);

        // hotkey
        cfg.hotkey_bind = tbl["hotkey"]["bind"].value_or(cfg.hotkey_bind);

        // inject
        cfg.type_delay_ms = tbl["inject"]["type_delay_ms"].value_or(cfg.type_delay_ms);

        // audio
        cfg.audio_device = tbl["audio"]["device"].value_or(cfg.audio_device);
        cfg.sample_rate  = tbl["audio"]["sample_rate"].value_or(cfg.sample_rate);

        std::cerr << "[config] Loaded from " << path << "\n";
    } catch (const toml::parse_error& err) {
        std::cerr << "[config] Parse error in " << path << ": " << err << "\n";
    }

    return cfg;
}
