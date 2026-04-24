#pragma once
#include <string>

// Inject (type) text at the current cursor position.
// Detects X11 vs Wayland and uses the appropriate method.
void inject_text(const std::string& text);

// Copy text to the system clipboard without pasting.
void inject_clipboard(const std::string& text);
