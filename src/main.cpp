#include "config.h"
#include "audio.h"
#include "hotkey.h"
#include "transcribe.h"
#include "inject.h"
#include "editor.h"
#include "settings.h"
#include "tray.h"

#include <gtk/gtk.h>
#include <glib-unix.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <csignal>
#include <thread>
#include <functional>
#include <mutex>
#include <unistd.h>

// ── Global state ────────────────────────────────────────────────────
static Config       g_cfg;
static AudioRecorder g_audio;
static HotkeyListener g_hotkey;
static Transcriber  g_whisper;
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

// Whisper writes non-speech sounds in brackets, e.g. "[BLANK_AUDIO]".
static bool is_non_speech(const std::string& text) {
    return text.size() >= 2 &&
           ((text.front() == '[' && text.back() == ']') ||
            (text.front() == '(' && text.back() == ')') ||
            (text.front() == '*' && text.back() == '*'));
}

// ── Transcription queue ─────────────────────────────────────────────
// One worker thread transcribes the recordings in the order of push().
namespace queue {

struct Job {
    unsigned session;                           // skipped if no longer current
    std::vector<float> pcm;
    std::string context;                        // text before the cursor
    std::function<void(std::string)> done;      // called on the worker thread
};

static std::mutex mtx;
static std::condition_variable cv;
static std::deque<Job> jobs;
static bool stopping = false;
static std::thread worker;
static std::atomic<unsigned> current_session{0};

static void start() {
    worker = std::thread([] {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [] { return stopping || !jobs.empty(); });
                if (stopping) return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            if (job.session != current_session.load()) continue;
            std::string text = g_whisper.transcribe(job.pcm, job.context);
            if (is_non_speech(text)) text.clear();
            job.done(std::move(text));
        }
    });
}

static void push(Job job) {
    {
        std::lock_guard<std::mutex> lk(mtx);
        jobs.push_back(std::move(job));
    }
    cv.notify_one();
}

static void stop() {
    {
        std::lock_guard<std::mutex> lk(mtx);
        stopping = true;
    }
    cv.notify_one();
    if (worker.joinable()) worker.join();
}

} // namespace queue

// ── Editor session ──────────────────────────────────────────────────
// The session state is used on the GTK main loop only.
//   Open:       the editor is shown; the user dictates and edits.
//   Finishing:  Type or Copy waits for the last transcripts.
//   Delivering: the editor is closed and its text is still typed.
enum class State { Idle, Open, Finishing, Delivering };
enum class Output { Type, Copy };

constexpr guint kTailMs = 250;     // recording after the talk key is released
constexpr gint64 kMinHoldUs = 200'000;  // shorter presses of the talk key are dropped

static State         g_state = State::Idle;
static Output        g_output = Output::Type;
static unsigned      g_session = 0;         // increments for each new or cancelled session
static unsigned long g_target_window = 0;   // window that had the focus before the editor
static bool          g_talking = false;     // the talk key is held
static guint         g_tail_timer = 0;      // records the tail after the talk key
static int           g_pending = 0;         // recordings that wait for their transcript
static gint64        g_held_us = 0;         // time the talk key was held for the capture
static gint64        g_talk_since = 0;      // start of the current press of the talk key

// Hide the editor and give the focus back to the window that had it before.
// Then type `text` there, if not empty.
static void close_editor(const std::string& text_to_type) {
    g_audio.stop();
    g_editor.hide();
    g_state = State::Delivering;
    g_tray.set_recording(false);
    unsigned long target = g_target_window;
    int delay = g_cfg.type_delay_ms;
    std::thread([text_to_type, target, delay]() {
        usleep(100'000);  // let the editor window unmap
        activate_window(target);
        inject_text(text_to_type, delay);
        run_on_main([] {
            if (g_state == State::Delivering) g_state = State::Idle;
        });
    }).detach();
}

static void deliver() {
    std::string text = g_editor.text();
    std::cerr << "[app] Editor session done (" << text.size() << " chars)\n";

    if (g_output == Output::Copy) {
        if (!text.empty()) {
            GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
            gtk_clipboard_set_text(clipboard, text.c_str(), -1);
            gtk_clipboard_store(clipboard);
            std::cerr << "[app] Copied to clipboard\n";
        }
        close_editor({});
        return;
    }
    close_editor(text);
}

// End the capture and queue it for transcription.
static void submit_capture() {
    if (g_tail_timer) {
        g_source_remove(g_tail_timer);
        g_tail_timer = 0;
    }
    std::vector<float> pcm = g_audio.end_capture();
    gint64 held = g_held_us;
    g_held_us = 0;
    if (g_audio.open_failed()) {
        g_editor.set_note("The microphone could not be opened.");
        return;
    }
    if (held < kMinHoldUs || pcm.empty()) return;

    unsigned session = g_session;
    ++g_pending;
    g_editor.set_pending(g_pending);
    queue::push({session, std::move(pcm), g_editor.context(), [session](std::string text) {
        run_on_main([session, text]() {
            if (session != g_session) return;
            --g_pending;
            g_editor.set_pending(g_pending);
            if (!text.empty()) g_editor.insert_transcript(text);
            if (g_state == State::Finishing && g_pending == 0) deliver();
        });
    }});
}

static void talk_start() {
    if (g_state != State::Open || g_talking) return;
    g_talking = true;
    g_talk_since = g_get_monotonic_time();
    // A new press during the tail continues the same recording.
    if (g_tail_timer) {
        g_source_remove(g_tail_timer);
        g_tail_timer = 0;
    } else {
        g_audio.begin_capture();
    }
    g_editor.set_talking(true);
    g_tray.set_recording(true);
}

static void talk_stop() {
    if (!g_talking) return;
    g_talking = false;
    g_held_us += g_get_monotonic_time() - g_talk_since;
    g_editor.set_talking(false);
    g_tray.set_recording(false);
    g_tail_timer = g_timeout_add(kTailMs, +[](gpointer) -> gboolean {
        g_tail_timer = 0;
        submit_capture();
        return G_SOURCE_REMOVE;
    }, nullptr);
}

static void open_editor() {
    queue::current_session = ++g_session;
    g_target_window = active_window();
    g_state = State::Open;
    g_talking = false;
    g_pending = 0;
    g_held_us = 0;
    g_editor.show();
    g_audio.configure(g_cfg);
    g_audio.start();
    std::cerr << "[app] Editor session started\n";
}

static void finish(Output output) {
    if (g_state != State::Open) return;
    g_output = output;
    g_state = State::Finishing;
    g_editor.set_finishing();
    if (g_talking || g_tail_timer) {
        talk_stop();
        submit_capture();
    }
    if (g_pending == 0) deliver();
}

static void cancel() {
    if (g_state == State::Idle || g_state == State::Delivering) return;
    queue::current_session = ++g_session;
    if (g_tail_timer) {
        g_source_remove(g_tail_timer);
        g_tail_timer = 0;
    }
    g_talking = false;
    close_editor({});
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

// ── Hotkey ──────────────────────────────────────────────────────────
// The hotkey opens the editor, or closes it and types the text.
static void on_toggle() {
    run_on_main([] {
        if (g_state == State::Idle) open_editor();
        else if (g_state == State::Open) finish(Output::Type);
    });
}

// ── GTK setup ───────────────────────────────────────────────────────
static void setup_gtk() {
    g_editor.init(g_cfg, {
        talk_start,
        talk_stop,
        [] { finish(Output::Type); },
        [] { finish(Output::Copy); },
        cancel,
        open_settings,
    });

    g_settings.init(g_config_path, apply_settings);

    g_tray.init({
        on_toggle,
        open_settings,
        [] { gtk_main_quit(); },
    }, "psst — voice to text");

    g_audio.set_chunk_callback([](const float* data, size_t count) {
        g_editor.push_samples(data, count);
    });

    g_hotkey.start();

    std::cerr << "[app] Ready — press " << g_cfg.hotkey_bind
              << " to open the editor\n";
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

    // Write PID file for --toggle and --settings
    {
        std::string pidfile = "/tmp/psst.pid";
        FILE* f = fopen(pidfile.c_str(), "w");
        if (f) {
            fprintf(f, "%d", (int)getpid());
            fclose(f);
        }
    }

    // SIGUSR1 → like the hotkey, SIGUSR2 → open the settings. GLib
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

    // Set up the windows, the tray icon and the callbacks
    queue::start();
    setup_gtk();

    // GTK main loop (blocks until gtk_main_quit)
    gtk_main();

    // Cleanup
    std::cerr << "[app] Shutting down...\n";
    g_hotkey.stop();
    g_audio.shutdown();
    queue::stop();
    g_whisper.shutdown();

    // Remove PID file
    std::remove("/tmp/psst.pid");

    return 0;
}
