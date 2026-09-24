#include "punctuate.h"
#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

const std::map<std::string, std::string>& default_punctuation_words() {
    static const std::map<std::string, std::string> words = {
        // English
        {"period",            ". "},
        {"full stop",         ". "},
        {"comma",             ", "},
        {"colon",             ": "},
        {"semicolon",         "; "},
        {"question mark",     "? "},
        {"exclamation mark",  "! "},
        {"exclamation point", "! "},
        {"dash",              " – "},
        {"hyphen",            "-"},
        {"new line",          "\n"},
        {"newline",           "\n"},
        {"new paragraph",     "\n\n"},
        {"open bracket",      " ("},
        {"close bracket",     ") "},
        {"open parenthesis",  " ("},
        {"close parenthesis", ") "},
        // German
        {"punkt",             ". "},
        {"komma",             ", "},
        {"doppelpunkt",       ": "},
        {"semikolon",         "; "},
        {"strichpunkt",       "; "},
        {"fragezeichen",      "? "},
        {"ausrufezeichen",    "! "},
        {"gedankenstrich",    " – "},
        {"bindestrich",       "-"},
        {"neue zeile",        "\n"},
        {"neuer absatz",      "\n\n"},
        {"klammer auf",       " ("},
        {"klammer zu",        ") "},
    };
    return words;
}

static std::string ascii_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// ASCII alphanumerics and all UTF-8 bytes count as word characters, so that
// non-ASCII letters (ä, ö, ü, ß, ...) never form a word boundary.
static bool is_word_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c >= 0x80;
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Marks that Whisper inserts at speech pauses around a spoken command.
static bool is_pause_mark(char c) {
    return c != '\0' && std::strchr(",.;:!?", c) != nullptr;
}

// Symbols that attach to the preceding word and absorb its pause marks.
static bool attaches_left(const std::string& value) {
    auto pos = value.find_first_not_of(' ');
    if (pos == std::string::npos) return false;
    static const char* marks[] = {".", ",", ":", ";", "!", "?", ")", "-", "–"};
    for (const char* m : marks)
        if (value.compare(pos, std::strlen(m), m) == 0) return true;
    return false;
}

static bool ends_sentence(const std::string& value) {
    auto pos = value.find_last_not_of(' ');
    if (pos == std::string::npos) return false;
    char c = value[pos];
    return c == '.' || c == '!' || c == '?' || c == '\n';
}

// Uppercase the letter at `text[i]`: ASCII, plus the UTF-8 umlauts ä, ö, ü.
static void capitalize_at(std::string& text, size_t i) {
    char& c = text[i];
    if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
    } else if (static_cast<unsigned char>(c) == 0xC3 && i + 1 < text.size()) {
        unsigned char& n = reinterpret_cast<unsigned char&>(text[i + 1]);
        if (n == 0xA4 || n == 0xB6 || n == 0xBC) n -= 0x20;
    }
}

// Collapse repeated spaces, drop spaces around line breaks, trim both ends.
static std::string tidy_spaces(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == ' ' && (out.empty() || out.back() == ' ' || out.back() == '\n'))
            continue;
        if (c == '\n')
            while (!out.empty() && out.back() == ' ') out.pop_back();
        out += c;
    }
    auto start = out.find_first_not_of(" \t\n\r");
    auto end   = out.find_last_not_of(" \t\r");
    if (start == std::string::npos) return {};
    return out.substr(start, end - start + 1);
}

std::string apply_punctuation(const std::string& text,
                              const std::map<std::string, std::string>& overrides) {
    std::map<std::string, std::string> merged = default_punctuation_words();
    for (const auto& [phrase, value] : overrides)
        merged[ascii_lower(phrase)] = value;

    // Longest phrases first, so "new line" wins over "line"-like prefixes.
    std::vector<std::pair<std::string, std::string>> words;
    for (const auto& [phrase, value] : merged)
        if (!phrase.empty() && !value.empty()) words.emplace_back(phrase, value);
    std::sort(words.begin(), words.end(), [](const auto& a, const auto& b) {
        return a.first.size() > b.first.size();
    });

    const std::string lower = ascii_lower(text);
    std::string out;
    bool capitalize_next = false;
    size_t i = 0;

    while (i < text.size()) {
        const std::pair<std::string, std::string>* match = nullptr;
        if (i == 0 || !is_word_char(static_cast<unsigned char>(text[i - 1]))) {
            for (const auto& w : words) {
                size_t end = i + w.first.size();
                if (lower.compare(i, w.first.size(), w.first) == 0 &&
                    (end == text.size() ||
                     !is_word_char(static_cast<unsigned char>(text[end])))) {
                    match = &w;
                    break;
                }
            }
        }

        if (!match) {
            if (capitalize_next && !is_space(text[i])) {
                size_t n = (static_cast<unsigned char>(text[i]) == 0xC3 &&
                            i + 1 < text.size()) ? 2 : 1;
                out += text.substr(i, n);
                capitalize_at(out, out.size() - n);
                capitalize_next = false;
                i += n;
            } else {
                out += text[i++];
            }
            continue;
        }

        const std::string& value = match->second;

        // Drop whitespace (and pause marks, for left-attaching symbols)
        // between the preceding word and the command.
        while (!out.empty() && is_space(out.back())) out.pop_back();
        if (attaches_left(value)) {
            while (!out.empty() && (is_pause_mark(out.back()) || is_space(out.back())))
                out.pop_back();
        }

        out += value;

        // Skip pause marks and whitespace after the command.
        i += match->first.size();
        while (i < text.size() && is_pause_mark(text[i])) ++i;
        while (i < text.size() && is_space(text[i])) ++i;

        capitalize_next = ends_sentence(value);
    }

    return tidy_spaces(out);
}
