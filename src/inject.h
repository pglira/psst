#pragma once
#include <string>

// Inject (type) text at the current cursor position.
// Detects X11 vs Wayland and uses the appropriate method.
// type_delay_ms: inter-keystroke delay on X11 (Wayland uses wtype's native pace).
void inject_text(const std::string& text, int type_delay_ms);

// Return the X11 window that has the input focus, or 0 if unknown.
unsigned long active_window();

// Give the input focus back to X11 window `window`, as returned by
// active_window(). Does nothing for window 0.
void activate_window(unsigned long window);

