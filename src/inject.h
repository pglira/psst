#pragma once
#include <string>

// Inject (type) text at the current cursor position.
// Detects X11 vs Wayland and uses the appropriate method.
// type_delay_ms: inter-keystroke delay on X11 (Wayland uses wtype's native pace).
void inject_text(const std::string& text, int type_delay_ms);

// Copy text to the system clipboard without pasting.
void inject_clipboard(const std::string& text);
