# psst

Voice-to-text for Linux using [whisper.cpp](https://github.com/ggerganov/whisper.cpp).
Press a hotkey to open a dictation window and speak. Each time you pause, the
text goes into an editable text field at its cursor. Then type the text into
the previous window, or copy it to the clipboard.

## Features

- **Global hotkey** (default: `Super+V`) to toggle recording
- **Real-time VU meter overlay** shown while recording (ESC to cancel)
- **GPU-accelerated** transcription via whisper.cpp (CUDA)
- **Dictation editor** — edit the text while you dictate; pause, type, or
  copy with buttons or key bindings
- **LLM correction** — correct the editor text with the claude CLI, with undo
- **Live typing** — without the editor, each utterance is typed into the
  focused window when you pause
- **Edit commands** — say "delete word" or "delete sentence" (or "Wort
  löschen", "Satz löschen") to remove text
- **Spoken punctuation** — say "colon", "dash", "new line" (or "Doppelpunkt",
  "Gedankenstrich", "neue Zeile") to insert `:`, `–`, a line break
- **Settings window** — change the settings in a window (`Ctrl+,` in the
  editor, the tray menu, or `psst --settings`); they apply at once
- **Tray icon** — click to start or stop dictation; menu for settings and quit
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

# Open the settings window of the running instance
./build/psst --settings
```

## Configuration

Config file location: `~/.config/psst/config.toml`

Copy the default config:
```bash
mkdir -p ~/.config/psst
cp config.toml ~/.config/psst/
```

See [config.toml](config.toml) for all options. The settings window writes
the same file; it keeps comments and the `[punctuation.words]` table.

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

### Dictation editor

With `[editor] enabled = true` (the default), the hotkey opens a window with a
text field. Each utterance goes in at the text cursor when you pause, so you
can click into the text or type corrections while you dictate.

| Key (default)       | Button | Action                                         |
|---------------------|--------|------------------------------------------------|
| `Ctrl+Space`        | Pause  | Pause or resume the microphone                 |
| `Ctrl+R`            | Correct | Correct the selection (or all text) with an LLM |
| `Ctrl+Z`            | Undo   | Undo the last correction                       |
| `Ctrl+Enter`        | Type   | Close and type the text into the previous window |
| `Ctrl+Shift+Enter`  | Copy   | Close and copy the text to the clipboard       |
| `Esc`               | Cancel | Close and discard the text                     |

Correct sends the selected text, or all text without a selection, to the
[claude CLI](https://docs.anthropic.com/en/docs/claude-code) (`claude -p`,
model `haiku` by default). It fixes recognition errors, spelling, grammar,
and punctuation, replaces words that sound alike but do not fit the context
(e.g. "cloud" → "Claude", "def container" → "devcontainer"), and keeps the
wording and the language. Dictation continues
during the correction; if the text in the range changes meanwhile, psst
discards the result. Set the command, model, and instructions in
`[correction]` or in the settings window.

The hotkey (`Super+V`) does the same as Type. Change the key bindings in
`[editor]`. On Wayland, psst cannot give the focus back to the previous
window; Type relies on the compositor to do it.

### Live typing and edit commands

With `[editor] enabled = false` and `[stream] enabled = true`, psst types
each utterance into the focused window when you pause for `pause_ms` (600 ms). Commands work across utterances: "comma"
at the start of an utterance replaces the period that Whisper put at the end
of the previous one.

| Say                          | Result                                  |
|------------------------------|-----------------------------------------|
| "delete word" / "Wort löschen"     | Removes the last word and its punctuation |
| "delete sentence" / "Satz löschen" | Removes the last sentence             |

In the editor, the edit commands act on the text before the cursor.

psst edits with BackSpace key presses and only knows the text that it typed
in the current recording. Do not move the cursor or type while you dictate,
and the edit commands do not reach text from before the recording.

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
│               │ (VU meter)  │  │ (type text) │   │
│               └─────────────┘  └─────────────┘   │
└──────────────────────────────────────────────────┘
```
