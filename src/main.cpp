#include "config.h"
#include "audio.h"
#include "hotkey.h"
#include "overlay.h"
#include "transcribe.h"
#include "inject.h"
#include "punctuate.h"
#include "stream.h"
#include "editor.h"
#include "settings.h"
#include "tray.h"

#include <gtk/gtk.h>
#include <glib-unix.h>
#include <iostream>
#include <csignal>
#include <filesystem>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>
#include <unistd.h>

namespace fs = std::filesystem;

// ── Global state ────────────────────────────────────────────────────
static Config       g_cfg;
static AudioRecorder g_audio;
static HotkeyListener g_hotkey;
static OverlayWindow g_overlay;
static Transcriber  g_whisper;
static std::atomic<bool> g_recording{false};
static LiveDictation g_live;
static EditorWindow g_editor;
static SettingsWindow g_settings;
static TrayIcon g_tray;
static std::string g_config_path;

// Run `fn` on the GTK main loop.
static void run_on_main(std::function<void()> fn) {
    g_idle_add(+[](gpointer data) -> gboolean {
        auto* f = static_cast<std::function<void()>*>(data);
        (*f)();
        delete f;
        return FALSE;
    }, new std::function<void()>(std::move(fn)));
}

// ── Live dictation ──────────────────────────────────────────────────
// Whisper writes non-speech sounds in brackets, e.g. "[BLANK_AUDIO]".
static bool is_non_speech(const std::string& text) {
    return text.size() >= 2 &&
           ((text.front() == '[' && text.back() == ']') ||
            (text.front() == '(' && text.back() == ')') ||
            (text.front() == '*' && text.back() == '*'));
}

// Start a live session: each utterance is transcribed and typed while the
// recording continues. `typed` holds the text typed in this session.
static void start_live_session() {
    auto typed = std::make_shared<std::string>();
    Config cfg = g_cfg;

    auto fetch = [](size_t from) { return g_audio.copy_from(from); };

    auto handle = [typed, cfg](const std::vector<float>& pcm,
                               const std::atomic<bool>& cancelled) {
        std::string text = g_whisper.transcribe(pcm, *typed);
        if (text.empty() || is_non_speech(text) || cancelled.load()) return;

        std::string next = append_transcript(*typed, text, cfg.punctuation_words,
                                             cfg.punctuation_enabled);
        std::cerr << "[punctuate] Result: \"" << next << "\"\n";
        inject_replace(*typed, next, cfg.type_delay_ms);
        *typed = std::move(next);
    };

    auto done = [typed, cfg](bool cancelled) {
        std::cerr << "[app] Live session " << (cancelled ? "cancelled" : "done")
                  << " (" << typed->size() << " chars)\n";
        if (cfg.copy_to_clipboard && !cancelled && !typed->empty())
            inject_clipboard(*typed);
    };

    g_live.start(g_cfg, fetch, handle, done);
}

// ── Editor session ──────────────────────────────────────────────────
// The editor state is used on the GTK main loop only.
// Delivering: the editor is closed and its text is still typed.
enum class EditorState { Idle, Recording, Paused, Finishing, Delivering };
enum class EditorOutput { Type, Copy };

static EditorState   g_editor_state = EditorState::Idle;
static EditorOutput  g_editor_output = EditorOutput::Type;
static unsigned      g_editor_session = 0;  // increments for each new or cancelled session
static unsigned long g_target_window = 0;   // window that had the focus before the editor

// Hide the editor and give the focus back to the window that had it before.
// Then type `text` there, if not empty.
static void editor_close(const std::string& text_to_type) {
    g_editor.hide();
    g_editor_state = EditorState::Delivering;
    g_tray.set_recording(false);
    unsigned long target = g_target_window;
    int delay = g_cfg.type_delay_ms;
    bool copy = g_cfg.copy_to_clipboard;
    std::thread([text_to_type, target, delay, copy]() {
        usleep(100'000);  // let the editor window unmap
        activate_window(target);
        if (!text_to_type.empty()) {
            inject_text(text_to_type, delay);
            if (copy) inject_clipboard(text_to_type);
        }
        run_on_main([] {
            if (g_editor_state == EditorState::Delivering)
                g_editor_state = EditorState::Idle;
        });
    }).detach();
}

static void editor_deliver() {
    std::string text = g_editor.text();
    std::cerr << "[app] Editor session done (" << text.size() << " chars)\n";

    if (g_editor_output == EditorOutput::Copy) {
        if (!text.empty()) {
            GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
            gtk_clipboard_set_text(clipboard, text.c_str(), -1);
            gtk_clipboard_store(clipboard);
            std::cerr << "[app] Copied to clipboard\n";
        }
        editor_close({});
        return;
    }
    editor_close(text);
}

static void editor_start() {
    unsigned session = ++g_editor_session;
    g_target_window = active_window();
    g_editor_state = EditorState::Recording;
    g_editor.show();
    g_audio.configure(g_cfg);
    g_audio.start();
    g_tray.set_recording(true);

    auto fetch = [](size_t from) { return g_audio.copy_from(from); };

    auto handle = [session](const std::vector<float>& pcm,
                            const std::atomic<bool>& cancelled) {
        std::string text = g_whisper.transcribe(pcm, g_editor.context());
        if (text.empty() || is_non_speech(text) || cancelled.load()) return;
        run_on_main([session, text]() {
            if (session != g_editor_session ||
                (g_editor_state != EditorState::Recording &&
                 g_editor_state != EditorState::Paused &&
                 g_editor_state != EditorState::Finishing)) return;
            g_editor.insert_transcript(text);
        });
    };

    auto done = [session](bool cancelled) {
        if (cancelled) return;
        run_on_main([session]() {
            if (session == g_editor_session && g_editor_state == EditorState::Finishing)
                editor_deliver();
        });
    };

    g_live.start(g_cfg, fetch, handle, done);
    std::cerr << "[app] Editor session started\n";
}

static void editor_finish(EditorOutput output) {
    if (g_editor_state != EditorState::Recording && g_editor_state != EditorState::Paused)
        return;
    g_editor_output = output;
    g_editor_state = EditorState::Finishing;
    g_editor.set_finishing();
    g_live.finish(g_audio.stop());
}

static void editor_pause() {
    if (g_editor_state == EditorState::Recording) {
        g_audio.set_paused(true);
        g_live.flush();
        g_editor_state = EditorState::Paused;
        g_editor.set_paused(true);
    } else if (g_editor_state == EditorState::Paused) {
        g_audio.set_paused(false);
        g_editor_state = EditorState::Recording;
        g_editor.set_paused(false);
    }
}

static void editor_cancel() {
    if (g_editor_state == EditorState::Idle ||
        g_editor_state == EditorState::Delivering) return;
    ++g_editor_session;
    g_audio.cancel();
    g_live.cancel();
    editor_close({});
    std::cerr << "[app] Editor session cancelled\n";
}

// ── Settings ────────────────────────────────────────────────────────
// Use saved settings at once. A changed model loads in the background.
static void apply_settings(const Config& cfg) {
    Config old = g_cfg;
    g_cfg = cfg;
    g_editor.reconfigure(cfg);

    std::string message = "Saved.";
    if (cfg.hotkey_bind != old.hotkey_bind) {
        if (g_hotkey.rebind(cfg.hotkey_bind)) {
            message += " Hotkey is now " + cfg.hotkey_bind + ".";
        } else {
            message += " The hotkey " + cfg.hotkey_bind +
                       " is not valid or is taken by another program; " +
                       old.hotkey_bind + " stays.";
            g_cfg.hotkey_bind = old.hotkey_bind;
            save_config(g_config_path, g_cfg);
        }
    }

    bool new_model = cfg.model_size != old.model_size || cfg.model_path != old.model_path ||
                     cfg.gpu_enabled != old.gpu_enabled;
    if (new_model) message += " Loading the " + cfg.model_size + " model…";
    g_settings.set_message(message);

    std::thread([cfg = g_cfg, new_model, message]() {
        bool ok = g_whisper.reload(cfg);
        if (!new_model) return;
        run_on_main([ok, message, model = cfg.model_size]() {
            std::string base = message.substr(0, message.rfind(" Loading"));
            g_settings.set_message(base + (ok ? " The " + model + " model is loaded."
                                              : " Could not load the " + model + " model."));
        });
    }).detach();
}

static void open_settings() {
    g_settings.show(g_cfg);
}

// ── Toggle recording ────────────────────────────────────────────────
static void on_toggle() {
    // This is called from the hotkey thread. Schedule work on the GTK main loop.
    g_idle_add(+[](gpointer) -> gboolean {
        // A running session keeps its mode when the settings change it.
        bool editor_active = g_editor_state != EditorState::Idle;
        bool use_editor = editor_active || (!g_recording.load() && g_cfg.editor_enabled);
        if (use_editor) {
            // The hotkey opens the editor, or closes it and types the text.
            if (g_editor_state == EditorState::Idle) editor_start();
            else if (g_editor_state != EditorState::Delivering) editor_finish(EditorOutput::Type);
            return FALSE;
        }
        if (!g_recording.load()) {
            // Start recording
            g_recording.store(true);
            g_audio.configure(g_cfg);
            g_audio.start();
            g_overlay.show();
            g_tray.set_recording(true);
            if (g_cfg.stream_enabled)
                start_live_session();
            std::cerr << "[app] Recording started\n";
        } else {
            // Stop recording → transcribe → inject
            g_recording.store(false);
            g_overlay.hide();
            g_tray.set_recording(false);
            std::cerr << "[app] Recording stopped, transcribing...\n";

            auto samples = g_audio.stop();
            if (g_cfg.stream_enabled) {
                g_live.finish(std::move(samples));
                return FALSE;
            }
            if (samples.empty()) {
                std::cerr << "[app] No audio recorded\n";
                return FALSE;
            }

            // Run transcription in a thread to keep UI responsive
            std::thread([samples = std::move(samples), cfg = g_cfg]() {
                std::string text = g_whisper.transcribe(samples);
                if (cfg.punctuation_enabled && !text.empty()) {
                    text = apply_punctuation(text, cfg.punctuation_words);
                    std::cerr << "[punctuate] Result: \"" << text << "\"\n";
                }
                std::cerr << "[app] Transcription done (" << text.size() << " chars)\n";
                if (!text.empty()) {
                    inject_text(text, cfg.type_delay_ms);
                    if (cfg.copy_to_clipboard)
                        inject_clipboard(text);
                } else {
                    std::cerr << "[app] Empty transcription result\n";
                }

            }).detach();
        }
        return FALSE; // one-shot idle callback
    }, nullptr);
}

// ── Cancel recording (ESC) ──────────────────────────────────────────
static void on_cancel() {
    if (g_recording.load()) {
        g_recording.store(false);
        g_audio.cancel();
        g_live.cancel();
        g_overlay.hide();
        g_tray.set_recording(false);
        std::cerr << "[app] Recording cancelled\n";
    }
}

// ── GTK activate ────────────────────────────────────────────────────
static void setup_gtk() {
    // Init overlay
    g_overlay.init(g_cfg);
    g_overlay.set_esc_callback(on_cancel);

    g_editor.init(g_cfg, {
        editor_pause,
        [] { editor_finish(EditorOutput::Type); },
        [] { editor_finish(EditorOutput::Copy); },
        editor_cancel,
        open_settings,
    });

    g_settings.init(g_config_path, apply_settings);

    g_tray.init({
        on_toggle,
        open_settings,
        [] { gtk_main_quit(); },
    }, "psst — voice to text");

    // Audio chunk callback → meter of the overlay and the editor
    g_audio.set_chunk_callback([](const float* data, size_t count) {
        g_editor.push_samples(data, count);
        g_overlay.push_samples(data, count);
    });

    // Start hotkey listener
    g_hotkey.start();

    std::cerr << "[app] Ready — press " << g_cfg.hotkey_bind
              << " to start recording\n";
}

// ── main ────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    // Handle --toggle / --settings (send SIGUSR1 / SIGUSR2 to running instance)
    if (argc > 1 && (std::string(argv[1]) == "--toggle" ||
                     std::string(argv[1]) == "--settings")) {
        int sig = std::string(argv[1]) == "--toggle" ? SIGUSR1 : SIGUSR2;
        // Find PID from lock file
        std::string pidfile = "/tmp/psst.pid";
        FILE* f = fopen(pidfile.c_str(), "r");
        if (f) {
            int pid = 0;
            if (fscanf(f, "%d", &pid) == 1 && pid > 0) {
                kill((pid_t)pid, sig);
                std::cerr << "[app] Sent " << argv[1] << " to PID " << pid << "\n";
            }
            fclose(f);
        } else {
            std::cerr << "[app] No running instance found\n";
        }
        return 0;
    }

    // Load config
    std::string config_path = default_config_path();
    if (argc > 2 && std::string(argv[1]) == "--config")
        config_path = argv[2];

    g_cfg = load_config(config_path);
    g_config_path = config_path;

    // Write PID file for --toggle
    {
        std::string pidfile = "/tmp/psst.pid";
        FILE* f = fopen(pidfile.c_str(), "w");
        if (f) {
            fprintf(f, "%d", (int)getpid());
            fclose(f);
        }
    }

    // SIGUSR1 → toggle recording, SIGUSR2 → open the settings. GLib
    // delivers them on the main loop.
    g_unix_signal_add(SIGUSR1, +[](gpointer) -> gboolean {
        on_toggle();
        return G_SOURCE_CONTINUE;
    }, nullptr);
    g_unix_signal_add(SIGUSR2, +[](gpointer) -> gboolean {
        open_settings();
        return G_SOURCE_CONTINUE;
    }, nullptr);

    // Load whisper model (do this before GTK loop so it's ready)
    if (!g_whisper.init(g_cfg)) {
        std::cerr << "[app] Failed to load whisper model — exiting\n";
        return 1;
    }

    // Init audio recorder
    if (!g_audio.init(g_cfg)) {
        std::cerr << "[app] Failed to init audio — exiting\n";
        return 1;
    }

    // Init hotkey listener
    if (!g_hotkey.init(g_cfg, on_toggle)) {
        std::cerr << "[app] Failed to init hotkey listener.\n"
                  << "  You can still use: psst --toggle\n"
                  << "  (bind this command in your compositor/WM)\n";
    }

    // Init GTK
    gtk_init(&argc, &argv);

    // Set up overlay, tray, callbacks
    setup_gtk();

    // GTK main loop (blocks until gtk_main_quit)
    gtk_main();

    // Cleanup
    std::cerr << "[app] Shutting down...\n";
    g_hotkey.stop();
    g_audio.shutdown();
    g_live.cancel();
    g_live.join();
    g_whisper.shutdown();

    // Remove PID file
    std::remove("/tmp/psst.pid");

    return 0;
}
