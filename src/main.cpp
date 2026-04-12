#include "config.h"
#include "audio.h"
#include "hotkey.h"
#include "overlay.h"
#include "transcribe.h"
#include "inject.h"
#include "tray.h"

#include <gtk/gtk.h>
#include <iostream>
#include <csignal>
#include <filesystem>
#include <thread>
#include <atomic>

namespace fs = std::filesystem;

// ── Global state ────────────────────────────────────────────────────
static Config       g_cfg;
static AudioRecorder g_audio;
static HotkeyListener g_hotkey;
static OverlayWindow g_overlay;
static Transcriber  g_whisper;
static TrayIcon     g_tray;
static std::atomic<bool> g_recording{false};

// ── Toggle recording ────────────────────────────────────────────────
static void on_toggle() {
    // This is called from the hotkey thread. Schedule work on the GTK main loop.
    g_idle_add(+[](gpointer) -> gboolean {
        if (!g_recording.load()) {
            // Start recording
            g_recording.store(true);
            g_audio.start();
            g_overlay.show();
            g_tray.set_state(TrayIcon::State::Recording);
            std::cerr << "[app] Recording started\n";
        } else {
            // Stop recording → transcribe → inject
            g_recording.store(false);
            g_overlay.hide();
            g_tray.set_state(TrayIcon::State::Transcribing);
            std::cerr << "[app] Recording stopped, transcribing...\n";

            auto samples = g_audio.stop();
            if (samples.empty()) {
                std::cerr << "[app] No audio recorded\n";
                g_tray.set_state(TrayIcon::State::Idle);
                return FALSE;
            }

            // Run transcription in a thread to keep UI responsive
            std::thread([samples = std::move(samples)]() {
                std::string text = g_whisper.transcribe(samples);
                std::cerr << "[app] Transcription done, text='" << text
                          << "' (" << text.size() << " chars)\n";
                if (!text.empty()) {
                    inject_text(text);
                } else {
                    std::cerr << "[app] Empty transcription result\n";
                }
                // Update tray back to idle on main thread
                g_idle_add(+[](gpointer) -> gboolean {
                    g_tray.set_state(TrayIcon::State::Idle);
                    return FALSE;
                }, nullptr);
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
        g_overlay.hide();
        g_tray.set_state(TrayIcon::State::Idle);
        std::cerr << "[app] Recording cancelled\n";
    }
}

// ── CLI --toggle support (for Wayland compositor keybindings) ────────
static void on_toggle_signal(int) {
    on_toggle();
}

// ── GTK activate ────────────────────────────────────────────────────
static void setup_gtk() {
    // Init overlay
    g_overlay.init(g_cfg);
    g_overlay.set_esc_callback(on_cancel);

    // Audio chunk callback → overlay push
    g_audio.set_chunk_callback([](const float* data, size_t count) {
        g_overlay.push_samples(data, count);
    });

    // Start hotkey listener
    g_hotkey.start();

    // Init tray icon
    std::string icon_dir;
    // Try to find assets directory relative to executable
    {
        char buf[4096];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = '\0';
            fs::path exe_dir = fs::path(buf).parent_path();
            fs::path assets  = exe_dir / "assets";
            if (fs::is_directory(assets))
                icon_dir = assets.string();
        }
    }
    g_tray.init(g_cfg, icon_dir);
    g_tray.set_quit_callback([]() {
        gtk_main_quit();
    });

    std::cerr << "[app] Ready — press " << g_cfg.hotkey_bind
              << " to start recording\n";
}

// ── main ────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    // Handle --toggle flag (sends SIGUSR1 to running instance)
    if (argc > 1 && std::string(argv[1]) == "--toggle") {
        // Find PID from lock file
        std::string pidfile = "/tmp/psst.pid";
        FILE* f = fopen(pidfile.c_str(), "r");
        if (f) {
            int pid = 0;
            if (fscanf(f, "%d", &pid) == 1 && pid > 0) {
                kill((pid_t)pid, SIGUSR1);
                std::cerr << "[app] Sent toggle to PID " << pid << "\n";
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

    // Write PID file for --toggle
    {
        std::string pidfile = "/tmp/psst.pid";
        FILE* f = fopen(pidfile.c_str(), "w");
        if (f) {
            fprintf(f, "%d", (int)getpid());
            fclose(f);
        }
    }

    // SIGUSR1 → toggle recording
    std::signal(SIGUSR1, on_toggle_signal);

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
    g_whisper.shutdown();
    g_tray.shutdown();

    // Remove PID file
    std::remove("/tmp/psst.pid");

    return 0;
}
