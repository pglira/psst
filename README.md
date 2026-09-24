# psst

Voice-to-text for Linux using [whisper.cpp](https://github.com/ggerganov/whisper.cpp).
Press a hotkey to record, press it again to transcribe, text is pasted at cursor.

## Features

- **Global hotkey** (default: `Super+V`) to toggle recording
- **Real-time VU meter overlay** shown while recording (ESC to cancel)
- **GPU-accelerated** transcription via whisper.cpp (CUDA)
- **Paste at cursor** — transcribed text is typed into the focused window
- **Spoken punctuation** — say "colon", "dash", "new line" (or "Doppelpunkt",
  "Gedankenstrich", "neue Zeile") to insert `:`, `–`, a line break
- **Configurable** via TOML config file
- **Model loaded once** at startup — runs in background, always ready

## Dependencies

### Build

```bash
# Ubuntu/Debian
sudo apt install -y cmake build-essential pkg-config \
  libgtk-3-dev libpulse-dev \
  libxdo-dev libx11-dev

# For CUDA support (NVIDIA GPU)
# Ensure CUDA toolkit is installed (nvcc, libcublas, etc.)

# Fedora
sudo dnf install -y cmake gcc-c++ pkg-config \
  gtk3-devel pulseaudio-libs-devel \
  libxdo-devel libX11-devel
```

### Runtime

```bash
# For text injection (typing)
sudo apt install xdotool          # X11
sudo apt install wtype            # Wayland

# Optional — only needed if [output] copy_to_clipboard = true
sudo apt install xclip            # X11
sudo apt install wl-clipboard     # Wayland
```

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=ON
cmake --build build -j$(nproc)
```

Without CUDA (CPU only):
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=OFF
cmake --build build -j$(nproc)
```

## Usage

```bash
# First run — will auto-download the whisper model (~466 MB for 'small')
./build/psst

# With custom config
./build/psst --config /path/to/config.toml

# Toggle recording from another process (for Wayland WM keybindings)
./build/psst --toggle
```

## Configuration

Config file location: `~/.config/psst/config.toml`

Copy the default config:
```bash
mkdir -p ~/.config/psst
cp config.toml ~/.config/psst/
```

See [config.toml](config.toml) for all options.

### Spoken punctuation

Whisper punctuates by itself from pauses and intonation, but it rarely writes
colons, dashes, or brackets. psst replaces spoken commands after
transcription:

| Say                                 | Result        |
|-------------------------------------|---------------|
| "Note colon first item"             | `Note: first item` |
| "Wait dash what"                    | `Wait – what` |
| "E hyphen Mail"                     | `E-Mail`      |
| "see open bracket below close bracket" | `see (below)` |
| "Erstens Komma zweitens Punkt"      | `Erstens, zweitens.` |

The `[punctuation.words]` table adds commands or disables built-in ones.
Commands such as "Punkt", "period", or "Komma" also match the normal word
(e.g. "drei Komma fünf"); disable them if this is a problem.

A "new line" command types a Return key, which sends the message in many
chat applications.

## Wayland Support

Global hotkeys on pure Wayland (without XWayland) are not supported by the
X11 hotkey API. Workaround: bind a key in your compositor's config to run:

```
psst --toggle
```

Examples:
- **Hyprland**: `bind = SUPER, V, exec, psst --toggle`
- **Sway**: `bindsym Mod4+v exec psst --toggle`

## Architecture

```
┌──────────────────────────────────────────────────┐
│  main.cpp — GTK3 Application + GLib main loop    │
│                                                  │
│  ┌──────────┐  ┌───────────┐  ┌────────────────┐ │
│  │ hotkey   │→ │ audio     │→ │ transcribe     │ │
│  │ listener │  │ recorder  │  │ (whisper.cpp)  │ │
│  └──────────┘  └─────┬─────┘  └───────┬────────┘ │
│                      │                │          │
│               ┌──────▼──────┐  ┌──────▼──────┐   │
│               │ overlay     │  │ inject      │   │
│               │ (VU meter)  │  │ (paste text)│   │
│               └─────────────┘  └─────────────┘   │
└──────────────────────────────────────────────────┘
```
