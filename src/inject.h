#pragma once
#include <string>

// Inject (type) text at the current cursor position.
// Detects X11 vs Wayland and uses the appropriate method.
void inject_text(const std::string& text);
