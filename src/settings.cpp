#include "settings.h"
#include "inject.h"
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

#ifdef HAS_X11
#include <gdk/gdkx.h>
#endif

namespace {

GtkWidget* combo_with_entry(std::initializer_list<const char*> items) {
    GtkWidget* combo = gtk_combo_box_text_new_with_entry();
    for (const char* item : items)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), item);
    return combo;
}

std::string combo_text(GtkWidget* combo) {
    gchar* raw = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    std::string text = raw ? raw : "";
    g_free(raw);
    return text;
}

void set_combo_text(GtkWidget* combo, const std::string& text) {
    GtkWidget* entry = gtk_bin_get_child(GTK_BIN(combo));
    gtk_entry_set_text(GTK_ENTRY(entry), text.c_str());
}

GtkWidget* spin(double min, double max, double step) {
    GtkWidget* s = gtk_spin_button_new_with_range(min, max, step);
    gtk_widget_set_halign(s, GTK_ALIGN_START);
    return s;
}

GtkWidget* toggle() {
    GtkWidget* s = gtk_switch_new();
    gtk_widget_set_halign(s, GTK_ALIGN_START);
    return s;
}

bool active(GtkWidget* sw) { return gtk_switch_get_active(GTK_SWITCH(sw)); }
int value(GtkWidget* sp) { return gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(sp)); }

// PulseAudio sources that are microphones (no monitors of outputs).
std::vector<std::string> audio_sources() {
    std::vector<std::string> sources;
    FILE* proc = popen("pactl list short sources 2>/dev/null", "r");
    if (!proc) return sources;
    char line[1024];
    while (std::fgets(line, sizeof(line), proc)) {
        char name[512];
        if (std::sscanf(line, "%*s %511s", name) == 1 &&
            !g_str_has_suffix(name, ".monitor"))
            sources.emplace_back(name);
    }
    pclose(proc);
    return sources;
}

} // namespace

void SettingsWindow::init(const std::string& config_path, Apply apply) {
    path_ = config_path;
    apply_ = std::move(apply);

    window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window_), "psst settings");
    gtk_window_set_keep_above(GTK_WINDOW(window_), TRUE);
    gtk_window_set_position(GTK_WINDOW(window_), GTK_WIN_POS_CENTER);
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
    g_signal_connect(window_, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), nullptr);
    // Escape closes the window, unless a key field captures it.
    g_signal_connect(window_, "key-press-event",
                     G_CALLBACK(+[](GtkWidget* w, GdkEventKey* e, gpointer data) -> gboolean {
                         auto* self = static_cast<SettingsWindow*>(data);
                         if (e->keyval != GDK_KEY_Escape) return FALSE;
                         for (const KeyField* f : {&self->hotkey_, &self->pause_key_,
                                                   &self->type_key_, &self->copy_key_})
                             if (f->capturing) return FALSE;
                         gtk_widget_hide(w);
                         return TRUE;
                     }), this);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    gtk_container_add(GTK_CONTAINER(window_), box);

    grid_ = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid_), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid_), 14);
    gtk_box_pack_start(GTK_BOX(box), grid_, TRUE, TRUE, 0);

    add_section("Speech recognition");
    model_ = combo_with_entry({"tiny", "base", "small", "medium", "large-v3-turbo", "large-v3"});
    add_row("Model", model_, "A model that is not on disk is downloaded when you save.");
    language_ = combo_with_entry({"auto", "en", "de", "fr", "es", "it"});
    add_row("Language", language_, "Language code, or \"auto\" to detect it.");
    prompt_ = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(prompt_), 36);
    add_row("Initial prompt", prompt_, "Text that biases the style of the transcription.");
    translate_ = toggle();
    add_row("Translate to English", translate_);
    gpu_ = toggle();
    add_row("Use GPU", gpu_);

    add_section("Dictation");
    editor_ = toggle();
    add_row("Dictation editor", editor_,
            "Off: the hotkey types the dictated text directly at the cursor.");
    stream_ = toggle();
    add_row("Live typing without editor", stream_,
            "Without the editor: type each utterance when you pause. "
            "Off: type all text when the recording stops.");
    commands_ = toggle();
    add_row("Spoken commands", commands_,
            "Punctuation (\"comma\", \"new line\") and edits (\"delete word\").");
    pause_ms_ = spin(200, 3000, 50);
    add_row("Pause that ends an utterance (ms)", pause_ms_);
    max_utterance_ = spin(5, 30, 1);
    add_row("Longest utterance (s)", max_utterance_);

    add_section("Keys");
    hotkey_.hotkey = true;
    add_row("Hotkey (global)", hotkey_.button = gtk_button_new(),
            "Click, then press the new key combination. Escape keeps the old one.");
    add_row("Pause / resume", pause_key_.button = gtk_button_new());
    add_row("Type", type_key_.button = gtk_button_new());
    add_row("Copy", copy_key_.button = gtk_button_new());
    for (KeyField* f : {&hotkey_, &pause_key_, &type_key_, &copy_key_})
        bind_key_field(*f);

    add_section("Output and audio");
    type_delay_ = spin(0, 100, 1);
    add_row("Delay between typed keys (ms)", type_delay_);
    clipboard_ = toggle();
    add_row("Also copy typed text to clipboard", clipboard_);
    device_ = gtk_combo_box_text_new_with_entry();
    gtk_entry_set_placeholder_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(device_))),
                                   "default microphone");
    add_row("Microphone", device_, "PulseAudio source; empty uses the default microphone.");

    message_ = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(message_), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(message_), TRUE);
    gtk_box_pack_start(GTK_BOX(box), message_, FALSE, FALSE, 0);

    GtkWidget* buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(buttons), GTK_BUTTONBOX_END);
    gtk_box_set_spacing(GTK_BOX(buttons), 6);
    GtkWidget* close = gtk_button_new_with_label("Close");
    GtkWidget* save = gtk_button_new_with_label("Save");
    gtk_style_context_add_class(gtk_widget_get_style_context(save), "suggested-action");
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_widget_hide), window_);
    g_signal_connect_swapped(save, "clicked",
                             G_CALLBACK(+[](SettingsWindow* self) { self->save(); }), this);
    gtk_container_add(GTK_CONTAINER(buttons), close);
    gtk_container_add(GTK_CONTAINER(buttons), save);
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);

    gtk_widget_show_all(box);
}

void SettingsWindow::add_section(const char* title) {
    GtkWidget* label = gtk_label_new(nullptr);
    std::string markup = std::string("<b>") + title + "</b>";
    gtk_label_set_markup(GTK_LABEL(label), markup.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    if (row_ > 0) gtk_widget_set_margin_top(label, 10);
    gtk_grid_attach(GTK_GRID(grid_), label, 0, row_++, 2, 1);
}

GtkWidget* SettingsWindow::add_row(const char* label, GtkWidget* field, const char* tooltip) {
    GtkWidget* l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_set_margin_start(l, 12);
    if (tooltip) {
        gtk_widget_set_tooltip_text(l, tooltip);
        gtk_widget_set_tooltip_text(field, tooltip);
    }
    gtk_grid_attach(GTK_GRID(grid_), l, 0, row_, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_), field, 1, row_++, 1, 1);
    return field;
}

void SettingsWindow::show(const Config& cfg) {
    load(cfg);
    set_message("");
    gtk_widget_show(window_);
    gtk_window_present(GTK_WINDOW(window_));
#ifdef HAS_X11
    // Ask for the focus as a pager does; focus-stealing prevention ignores
    // a plain request from a window opened by a hotkey or the tray.
    GdkWindow* gdk_win = gtk_widget_get_window(window_);
    if (gdk_win && GDK_IS_X11_WINDOW(gdk_win)) {
        unsigned long xid = gdk_x11_window_get_xid(gdk_win);
        std::thread([xid]() { activate_window(xid); }).detach();
    }
#endif
}

void SettingsWindow::set_message(const std::string& text) {
    gtk_label_set_text(GTK_LABEL(message_), text.c_str());
}

void SettingsWindow::load(const Config& cfg) {
    base_ = cfg;
    set_combo_text(model_, cfg.model_size);
    set_combo_text(language_, cfg.language);
    gtk_entry_set_text(GTK_ENTRY(prompt_), cfg.initial_prompt.c_str());
    gtk_switch_set_active(GTK_SWITCH(translate_), cfg.translate);
    gtk_switch_set_active(GTK_SWITCH(gpu_), cfg.gpu_enabled);
    gtk_switch_set_active(GTK_SWITCH(editor_), cfg.editor_enabled);
    gtk_switch_set_active(GTK_SWITCH(stream_), cfg.stream_enabled);
    gtk_switch_set_active(GTK_SWITCH(commands_), cfg.punctuation_enabled);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(pause_ms_), cfg.stream_pause_ms);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(max_utterance_),
                              cfg.stream_max_utterance_ms / 1000.0);
    set_key(hotkey_, cfg.hotkey_bind);
    set_key(pause_key_, cfg.editor_pause_key);
    set_key(type_key_, cfg.editor_type_key);
    set_key(copy_key_, cfg.editor_copy_key);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(type_delay_), cfg.type_delay_ms);
    gtk_switch_set_active(GTK_SWITCH(clipboard_), cfg.copy_to_clipboard);

    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(device_));
    for (const auto& source : audio_sources())
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(device_), source.c_str());
    set_combo_text(device_, cfg.audio_device);
}

Config SettingsWindow::collect() const {
    Config cfg = base_;
    cfg.model_size = combo_text(model_);
    cfg.language = combo_text(language_);
    cfg.initial_prompt = gtk_entry_get_text(GTK_ENTRY(prompt_));
    cfg.translate = active(translate_);
    cfg.gpu_enabled = active(gpu_);
    cfg.editor_enabled = active(editor_);
    cfg.stream_enabled = active(stream_);
    cfg.punctuation_enabled = active(commands_);
    cfg.stream_pause_ms = value(pause_ms_);
    cfg.stream_max_utterance_ms = value(max_utterance_) * 1000;
    cfg.hotkey_bind = hotkey_.value;
    cfg.editor_pause_key = pause_key_.value;
    cfg.editor_type_key = type_key_.value;
    cfg.editor_copy_key = copy_key_.value;
    cfg.type_delay_ms = value(type_delay_);
    cfg.copy_to_clipboard = active(clipboard_);
    cfg.audio_device = combo_text(device_);
    if (cfg.model_size.empty()) cfg.model_size = base_.model_size;
    if (cfg.language.empty()) cfg.language = "auto";
    return cfg;
}

void SettingsWindow::save() {
    Config cfg = collect();
    if (!save_config(path_, cfg)) {
        set_message("Could not write " + path_);
        return;
    }
    base_ = cfg;
    set_message("Saved to " + path_);
    if (apply_) apply_(cfg);
}

// ── Key fields ──────────────────────────────────────────────────────

std::string SettingsWindow::key_label(const KeyField& field) {
    if (field.capturing) return "Press keys…";
    if (field.value.empty()) return "(none)";
    if (!field.hotkey) {
        guint keyval = 0;
        GdkModifierType mods;
        gtk_accelerator_parse(field.value.c_str(), &keyval, &mods);
        if (keyval == 0) return field.value;
        gchar* raw = gtk_accelerator_get_label(keyval, mods);
        std::string label = raw ? raw : field.value;
        g_free(raw);
        return label;
    }
    return field.value;
}

void SettingsWindow::set_key(KeyField& field, const std::string& value) {
    field.value = value;
    field.capturing = false;
    gtk_button_set_label(GTK_BUTTON(field.button), key_label(field).c_str());
}

void SettingsWindow::bind_key_field(KeyField& field) {
    gtk_widget_set_halign(field.button, GTK_ALIGN_START);
    gtk_widget_set_size_request(field.button, 220, -1);
    g_signal_connect(field.button, "clicked",
                     G_CALLBACK(+[](GtkButton* button, gpointer data) {
                         auto* f = static_cast<KeyField*>(data);
                         f->capturing = true;
                         gtk_button_set_label(button, key_label(*f).c_str());
                         gtk_widget_grab_focus(GTK_WIDGET(button));
                     }), &field);
    g_signal_connect(field.button, "key-press-event", G_CALLBACK(on_key_capture), &field);
    g_signal_connect(field.button, "focus-out-event",
                     G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
                         auto* f = static_cast<KeyField*>(data);
                         if (f->capturing) {
                             f->capturing = false;
                             gtk_button_set_label(GTK_BUTTON(f->button), key_label(*f).c_str());
                         }
                         return FALSE;
                     }), &field);
}

gboolean SettingsWindow::on_key_capture(GtkWidget*, GdkEventKey* event, gpointer data) {
    auto* f = static_cast<KeyField*>(data);
    if (!f->capturing) return FALSE;

    // Wait for the key that completes the combination.
    switch (event->keyval) {
    case GDK_KEY_Shift_L: case GDK_KEY_Shift_R: case GDK_KEY_Control_L:
    case GDK_KEY_Control_R: case GDK_KEY_Alt_L: case GDK_KEY_Alt_R:
    case GDK_KEY_Super_L: case GDK_KEY_Super_R: case GDK_KEY_Meta_L:
    case GDK_KEY_Meta_R: case GDK_KEY_ISO_Level3_Shift:
        return TRUE;
    case GDK_KEY_Escape:
        f->capturing = false;
        gtk_button_set_label(GTK_BUTTON(f->button), key_label(*f).c_str());
        return TRUE;
    default:
        break;
    }

    GdkModifierType state = GdkModifierType(event->state);
    gdk_keymap_add_virtual_modifiers(gdk_keymap_get_for_display(gdk_display_get_default()),
                                     &state);
    GdkModifierType mods = GdkModifierType(state & gtk_accelerator_get_default_mod_mask());
    guint keyval = gdk_keyval_to_lower(event->keyval);
    if (keyval == GDK_KEY_ISO_Left_Tab) keyval = GDK_KEY_Tab;

    std::string value;
    if (f->hotkey) {
        if (mods & GDK_SUPER_MASK)   value += "super+";
        if (mods & GDK_CONTROL_MASK) value += "ctrl+";
        if (mods & GDK_MOD1_MASK)    value += "alt+";
        if (mods & GDK_SHIFT_MASK)   value += "shift+";
        const char* name = gdk_keyval_name(keyval);
        value += name ? name : "";
    } else {
        gchar* raw = gtk_accelerator_name(keyval, mods);
        value = raw ? raw : "";
        g_free(raw);
    }
    f->value = value;
    f->capturing = false;
    gtk_button_set_label(GTK_BUTTON(f->button), key_label(*f).c_str());
    return TRUE;
}
