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
        // Edit commands (English and German)
        {"delete word",            kDeleteWord},
        {"delete last word",       kDeleteWord},
        {"delete sentence",        kDeleteSentence},
        {"delete last sentence",   kDeleteSentence},
        {"wort löschen",           kDeleteWord},
        {"letztes wort löschen",   kDeleteWord},
        {"satz löschen",           kDeleteSentence},
        {"letzten satz löschen",   kDeleteSentence},
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

static bool is_inline_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
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

// A word separator is needed between `text` and a following word, unless
// `text` is empty or ends in whitespace or an opening symbol.
static bool needs_separator(const std::string& text) {
    if (text.empty()) return false;
    char c = text.back();
    return !is_space(c) && c != '(' && c != '[' && c != '-' && c != '/';
}

// Remove trailing whitespace. Return true if it contained a line break.
static bool pop_spaces(std::string& out) {
    bool line_break = false;
    while (!out.empty() && is_space(out.back())) {
        line_break |= out.back() == '\n';
        out.pop_back();
    }
    return line_break;
}

// Remove the last word of `out`, together with its trailing pause marks.
// Without a word at the end, remove the last run of symbols instead.
// A trailing line break is removed alone.
static void delete_word(std::string& out) {
    if (pop_spaces(out)) return;
    while (!out.empty() && is_pause_mark(out.back())) out.pop_back();
    pop_spaces(out);
    size_t before = out.size();
    while (!out.empty() && is_word_char(static_cast<unsigned char>(out.back())))
        out.pop_back();
    if (out.size() == before) {
        while (!out.empty() && !is_space(out.back()) &&
               !is_word_char(static_cast<unsigned char>(out.back())))
            out.pop_back();
    }
    pop_spaces(out);
}

// Remove the last sentence of `out`: everything back to the previous
// sentence end (".", "!" or "?" before a space) or line break. A trailing line break is removed alone.
static void delete_sentence(std::string& out) {
    if (pop_spaces(out)) return;
    bool trailing_space = false;  // a mark followed by a space ends a sentence
    while (!out.empty() && is_pause_mark(out.back())) out.pop_back();
    while (!out.empty()) {
        char c = out.back();
        if (c == '\n') break;
        if (std::strchr(".!?", c) && trailing_space) break;
        trailing_space = is_space(c);
        out.pop_back();
    }
    pop_spaces(out);
}

// Collapse repeated spaces, drop spaces around line breaks, trim spaces at
// both ends.
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
    auto start = out.find_first_not_of(" \t\r");
    auto end   = out.find_last_not_of(" \t\r");
    if (start == std::string::npos) return {};
    return out.substr(start, end - start + 1);
}

std::string append_transcript(const std::string& buffer,
                              const std::string& text,
                              const std::map<std::string, std::string>& overrides,
                              bool commands) {
    std::map<std::string, std::string> merged = default_punctuation_words();
    for (const auto& [phrase, value] : overrides)
        merged[ascii_lower(phrase)] = value;

    // Longest phrases first, so "new line" wins over "line"-like prefixes.
    std::vector<std::pair<std::string, std::string>> words;
    if (commands) {
        for (const auto& [phrase, value] : merged)
            if (!phrase.empty() && !value.empty()) words.emplace_back(phrase, value);
    }
    std::sort(words.begin(), words.end(), [](const auto& a, const auto& b) {
        return a.first.size() > b.first.size();
    });

    const std::string lower = ascii_lower(text);
    std::string out = buffer;
    size_t i = text.find_first_not_of(" \t\r");
    if (i == std::string::npos) return buffer;
    if (needs_separator(out) && !is_pause_mark(text[i])) out += ' ';
    bool capitalize_next = !buffer.empty() && ends_sentence(buffer);

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
        i += match->first.size();
        while (i < text.size() && is_pause_mark(text[i])) ++i;
        while (i < text.size() && is_space(text[i])) ++i;

        if (value == kDeleteWord || value == kDeleteSentence) {
            if (value == kDeleteWord) delete_word(out);
            else delete_sentence(out);
            capitalize_next = out.empty() || ends_sentence(out);
            if (needs_separator(out)) out += ' ';
            continue;
        }

        // Drop whitespace (and pause marks, for left-attaching symbols)
        // between the preceding word and the command.
        while (!out.empty() && is_inline_space(out.back())) out.pop_back();
        if (attaches_left(value)) {
            while (!out.empty() &&
                   (is_pause_mark(out.back()) || is_inline_space(out.back())))
                out.pop_back();
        }

        out += value;
        capitalize_next = ends_sentence(value);
    }

    return tidy_spaces(out);
}

