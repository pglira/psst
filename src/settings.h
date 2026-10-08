#pragma once
#include "config.h"
#include <gtk/gtk.h>
#include <functional>
#include <string>

// Window to change the settings and save them to the config file.
class SettingsWindow {
public:
    // Called with the new settings after they are saved.
    using Apply = std::function<void(const Config& cfg)>;

    void init(const std::string& config_path, Apply apply);

    // Show the window with the values of `cfg` and give it the input focus.
    void show(const Config& cfg);

    // Show a message below the settings, e.g. the progress of a model load.
    void set_message(const std::string& text);

private:
    struct KeyField {
        GtkWidget* button = nullptr;
        std::string value;    // accelerator, or psst hotkey format
        bool hotkey = false;  // psst hotkey format ("super+v")
        bool capturing = false;
    };

    void load(const Config& cfg);
    Config collect() const;
    void save();

    GtkWidget* add_row(const char* label, GtkWidget* field, const char* tooltip = nullptr);
    void add_section(const char* title);
    void set_key(KeyField& field, const std::string& value);
    void bind_key_field(KeyField& field);
    static gboolean on_key_capture(GtkWidget* widget, GdkEventKey* event, gpointer data);
    static std::string key_label(const KeyField& field);

    std::string path_;
    Apply apply_;
    Config base_;  // settings that the window does not show stay as they are

    GtkWidget* window_ = nullptr;
    GtkWidget* grid_ = nullptr;
    GtkWidget* message_ = nullptr;
    int row_ = 0;

    GtkWidget* model_ = nullptr;
    GtkWidget* language_ = nullptr;
    GtkWidget* prompt_ = nullptr;
    GtkWidget* translate_ = nullptr;
    GtkWidget* gpu_ = nullptr;
    GtkWidget* commands_ = nullptr;
    GtkWidget* type_delay_ = nullptr;
    GtkWidget* device_ = nullptr;
    GtkWidget* correction_command_ = nullptr;
    GtkWidget* correction_model_ = nullptr;
    GtkTextBuffer* correction_prompt_ = nullptr;
    KeyField hotkey_, talk_key_, type_key_, copy_key_, correct_key_, undo_key_;
};
