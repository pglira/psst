#pragma once

// Play short iconic chirps to signal psst activation/deactivation.
// Sounds are synthesized on the fly and played asynchronously via PulseAudio,
// so calls return immediately and never block the GTK main loop.
//
// Enabled via the [sound] section of config.toml.

void sound_set_enabled(bool enabled);
void sound_play_activate();
void sound_play_deactivate();
