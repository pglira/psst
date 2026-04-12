# psst

Voice-to-text for Linux using [whisper.cpp](https://github.com/ggerganov/whisper.cpp).
Press a hotkey to record, release to transcribe, text is pasted at cursor.

## Features

- **Global hotkey** (default: `Super+V`) to toggle recording
- **Real-time spectrum overlay** shown while recording (ESC to cancel)
- **GPU-accelerated** transcription via whisper.cpp (CUDA)
- **Paste at cursor** — transcribed text is typed into the focused window
- **System tray icon** with status indicator
- **Configurable** via TOML config file
- **Model loaded once** at startup — runs in background, always ready

## Dependencies

### Build

```bash
# Ubuntu/Debian
sudo apt install -y cmake build-essential pkg-config \
  libgtk-4-dev libpulse-dev libfftw3-dev \
  libxdo-dev libx11-dev \
  libayatana-appindicator3-dev

# For CUDA support (NVIDIA GPU)
# Ensure CUDA toolkit is installed (nvcc, libcublas, etc.)

# Fedora
sudo dnf install -y cmake gcc-c++ pkg-config \
  gtk4-devel pulseaudio-libs-devel fftw-devel \
  libxdo-devel libX11-devel \
  libayatana-appindicator-gtk3-devel
```

### Runtime

```bash
# For text injection
sudo apt install xdotool xclip    # X11
sudo apt install wtype wl-clipboard  # Wayland
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
┌────────────────────────────────────────────────────┐
│  main.cpp — GTK4 Application + GLib main loop      │
│                                                     │
│  ┌──────────┐  ┌───────────┐  ┌──────────────────┐ │
│  │ hotkey   │→ │ audio     │→ │ transcribe       │ │
│  │ listener │  │ recorder  │  │ (whisper.cpp GPU) │ │
│  └──────────┘  └─────┬─────┘  └────────┬─────────┘ │
│                      │                  │           │
│               ┌──────▼──────┐    ┌──────▼──────┐   │
│               │ overlay     │    │ inject      │   │
│               │ (spectrum)  │    │ (paste text)│   │
│               └─────────────┘    └─────────────┘   │
│                                                     │
│  ┌──────────────────────────────────────────────┐   │
│  │ tray icon (ayatana-appindicator)             │   │
│  └──────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────┘
```
