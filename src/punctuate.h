#pragma once
#include <map>
#include <string>

// Command values that edit the text instead of inserting it.
inline constexpr const char* kDeleteWord     = "<delete-word>";
inline constexpr const char* kDeleteSentence = "<delete-sentence>";

// Built-in spoken punctuation commands (English and German).
// Keys are lowercase phrases, values are the inserted text including spacing.
const std::map<std::string, std::string>& default_punctuation_words();

// Append transcript `text` to `buffer`, the text already dictated, and return
// the combined text. Spoken commands in `text` become symbols and can edit
// `buffer`, e.g. remove its last word.
//
// Matching ignores ASCII case and requires word boundaries. Pause marks that
// Whisper places around a command (e.g. "Hello, colon, world") are dropped.
// A replacement that ends a sentence capitalizes the next letter.
//
// `overrides` adds to or replaces the built-in commands; an empty value
// disables the command with that phrase. The values kDeleteWord and
// kDeleteSentence remove the last word or sentence of the text so far.
// With `commands` false, `text` is only appended with a separating space.
std::string append_transcript(const std::string& buffer,
                              const std::string& text,
                              const std::map<std::string, std::string>& overrides,
                              bool commands);
