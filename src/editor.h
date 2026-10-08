#pragma once
#include "config.h"
#include "correct.h"
#include "meter.h"
#include <gtk/gtk.h>
#include <functional>
#include <mutex>
#include <string>

// Dictation window with an editable text field.
//
// The user holds the talk key (or the Talk button) to dictate; the
// transcript goes in at the text cursor, and the user can edit the text
// between dictations. Buttons and key bindings also correct the text with an
// LLM (and undo that), type the text into the previously focused window,
// copy it to the clipboard, or cancel.
class EditorWindow {
public:
    struct Actions {
        std::function<void()> talk_start;  // talk key or button pressed
        std::function<void()> talk_stop;   // talk key or button released
        std::function<void()> type;    // close and type the text
        std::function<void()> copy;    // close and copy the text
        std::function<void()> cancel;  // close and discard the text
        std::function<void()> settings; // open the settings window
    };

    void init(const Config& cfg, Actions actions);

    // Use the key bindings, spoken commands and correction settings of `cfg`.
    void reconfigure(const Config& cfg);


    // Show the window with an empty text field and give it the input focus.
    void show();
    void hide();

    // Feed the VU meter. Call from the audio thread.
    void push_samples(const float* data, size_t count);

    // Show whether the user talks.
    void set_talking(bool talking);

    // Show a short message next to the VU meter.
    void set_note(const std::string& text);

    // Show the number of recordings that wait for their transcript.
    void set_pending(int pending);

    // Disable the controls while the last recordings are transcribed.
    void set_finishing();

    // Insert the transcript of one recording at the text cursor. Spoken
    // commands apply to the text before the cursor.
    void insert_transcript(const std::string& transcript);

    // The whole text of the text field.
    std::string text() const;

    // The text before the cursor. Thread-safe.
    std::string context() const;

private:
    struct Key {
        guint keyval = 0;
        GdkModifierType mods = GdkModifierType(0);
    };

    static gboolean on_key(GtkWidget* widget, GdkEventKey* event, gpointer data);
    static gboolean on_tick(gpointer data);
    void update_context();
    enum class Phase { Ready, Talking, Finishing };
    static gboolean on_key_release(GtkWidget* widget, GdkEventKey* event, gpointer data);
    bool releases_talk_key(const GdkEventKey* event) const;
    void set_status(Phase phase);
    void update_labels();
    static gboolean on_draw_icon(GtkWidget* widget, cairo_t* cr, gpointer data);
    void set_button(GtkWidget* button, const char* name, const Key& key);

    // LLM correction of the selection, or of all text without a selection.
    void correct();
    void on_corrected(bool ok, const std::string& result);
    void undo_correction();
    // Replace the text between `from` and `to` with `text`; return the
    // character offsets of the new text.
    std::pair<int, int> replace_range(GtkTextIter* from, GtkTextIter* to,
                                      const std::string& text);
    std::string text_between(GtkTextMark* from, GtkTextMark* to) const;

    static Key parse_key(const std::string& accel);
    static bool matches(const Key& key, const GdkEventKey* event);
    static std::string label_of(const Key& key);
    static constexpr Key kEscape{GDK_KEY_Escape, GdkModifierType(0)};

    Config cfg_;
    Actions actions_;
    Key talk_key_, type_key_, copy_key_, correct_key_, undo_key_;

    GtkWidget* window_   = nullptr;
    GtkWidget* view_     = nullptr;
    GtkWidget* level_    = nullptr;  // VU meter
    GtkWidget* status_   = nullptr;  // state label
    GtkWidget* icon_     = nullptr;  // state icon
    Phase phase_  = Phase::Ready;
    GtkWidget* talk_btn_ = nullptr;
    GtkWidget* type_btn_  = nullptr;
    GtkWidget* copy_btn_  = nullptr;
    GtkWidget* cancel_btn_ = nullptr;
    GtkWidget* correct_btn_ = nullptr;
    GtkWidget* undo_btn_    = nullptr;
    GtkWidget* note_        = nullptr;  // messages, e.g. of the correction
    GtkTextBuffer* buffer_ = nullptr;
    guint timer_id_ = 0;

    LevelMeter meter_;
    int last_filled_ = -1;  // meter segments drawn last

    Corrector corrector_;
    // Range that the running correction covers, and its text at the start.
    GtkTextMark* correct_from_ = nullptr;
    GtkTextMark* correct_to_   = nullptr;
    std::string  correct_original_;
    // Range of the last correction, for undo.
    GtkTextMark* undo_from_ = nullptr;
    GtkTextMark* undo_to_   = nullptr;
    std::string  undo_original_, undo_corrected_;
    bool         undo_available_ = false;

    mutable std::mutex context_mtx_;
    std::string context_;
};
