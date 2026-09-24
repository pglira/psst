#pragma once
#include <map>
#include <string>

// Built-in spoken punctuation commands (English and German).
// Keys are lowercase phrases, values are the inserted text including spacing.
const std::map<std::string, std::string>& default_punctuation_words();

// Replace spoken punctuation commands in a transcript with their symbols.
//
// Matching ignores ASCII case and requires word boundaries. Pause marks that
// Whisper places around a command (e.g. "Hello, colon, world") are dropped.
// A replacement that ends a sentence capitalizes the next letter.
//
// `overrides` adds to or replaces the built-in commands; an empty value
// disables the command with that phrase.
std::string apply_punctuation(const std::string& text,
                              const std::map<std::string, std::string>& overrides);
