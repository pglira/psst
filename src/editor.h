#pragma once
#include "config.h"
#include "overlay.h"
#include <gtk/gtk.h>
#include <functional>
#include <mutex>
#include <string>

// Dictation window with an editable text field.
//
// Dictated text goes in at the text cursor, and the user can edit the text
// while dictating. Buttons and key bindings pause the recording, type the
// text into the previously focused window, copy it to the clipboard, or
// cancel.
class EditorWindow {
public:
    struct Actions {
        std::function<void()> pause;   // pause or resume the recording
        std::function<void()> type;    // close and type the text
        std::function<void()> copy;    // close and copy the text
        std::function<void()> cancel;  // close and discard the text
        std::function<void()> settings; // open the settings window
    };

    void init(const Config& cfg, Actions actions);

    // Use the key bindings and spoken commands of `cfg`.
    void reconfigure(const Config& cfg);

    // The X11 window ID of the editor, or 0.
    unsigned long xid() const;

    // Show the window with an empty text field and give it the input focus.
    void show();
    void hide();
    bool is_visible() const;

    // Feed the VU meter. Call from the audio thread.
    void push_samples(const float* data, size_t count);

    void set_paused(bool paused);

    // Disable the controls while the last utterances are transcribed.
    void set_finishing();

    // Insert the transcript of one utterance at the text cursor. Spoken
    // commands apply to the text before the cursor.
    void insert_transcript(const std::string& transcript);

    // The whole text of the text field.
    std::string text() const;

    // The text before the cursor. Thread-safe.
    std::string context() const;

private:
    static gboolean on_key(GtkWidget* widget, GdkEventKey* event, gpointer data);
    static gboolean on_tick(gpointer data);
    void update_context();
    enum class Phase { Recording, Paused, Finishing };
    void set_status(Phase phase);
    void update_labels();
    static gboolean on_draw_icon(GtkWidget* widget, cairo_t* cr, gpointer data);
    std::string pause_label(bool paused) const;

    struct Key {
        guint keyval = 0;
        GdkModifierType mods = GdkModifierType(0);
    };
    static Key parse_key(const std::string& accel);
    static bool matches(const Key& key, const GdkEventKey* event);
    static std::string label_of(const Key& key);

    Config cfg_;
    Actions actions_;
    Key pause_key_, type_key_, copy_key_;

    GtkWidget* window_   = nullptr;
    GtkWidget* view_     = nullptr;
    GtkWidget* level_    = nullptr;  // VU meter
    GtkWidget* status_   = nullptr;  // state label
    GtkWidget* icon_     = nullptr;  // state icon
    Phase phase_  = Phase::Recording;
    GtkWidget* pause_btn_ = nullptr;
    GtkWidget* type_btn_  = nullptr;
    GtkWidget* copy_btn_  = nullptr;
    GtkWidget* cancel_btn_ = nullptr;
    GtkTextBuffer* buffer_ = nullptr;
    guint timer_id_ = 0;

    LevelMeter meter_;
    int last_filled_ = -1;  // meter segments drawn last

    mutable std::mutex context_mtx_;
    std::string context_;
};
