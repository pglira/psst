#pragma once
#include <cstddef>
#include <string>

// Inject (type) text at the current cursor position.
// Detects X11 vs Wayland and uses the appropriate method.
// type_delay_ms: inter-keystroke delay on X11 (Wayland uses wtype's native pace).
void inject_text(const std::string& text, int type_delay_ms);

// Press BackSpace `backspaces` times, then type `text`.
void inject_edit(size_t backspaces, const std::string& text, int type_delay_ms);

// Change typed text `before` to `after`: erase the characters after their
// common prefix with BackSpace, then type the rest of `after`.
void inject_replace(const std::string& before, const std::string& after,
                    int type_delay_ms);

// Copy text to the system clipboard without pasting.
void inject_clipboard(const std::string& text);
