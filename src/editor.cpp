#include "editor.h"
#include "inject.h"
#include "punctuate.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <thread>

#ifdef HAS_X11
#include <gdk/gdkx.h>
#endif

namespace {
const char* kCss = R"(
window.psst-editor { background-color: #1f1f26; border: 3px solid #0072a3; }
window.psst-editor textview, window.psst-editor textview text {
    background-color: #2a2a33; color: #e8e8ee; font-size: 13pt; }
window.psst-editor .psst-rec { color: #ff4545; font-weight: bold; }
window.psst-editor .psst-paused { color: #ffb020; font-weight: bold; }
window.psst-editor .psst-status { color: #8a8a99; font-weight: bold; }
window.psst-editor .psst-note { color: #8a8a99; }
window.psst-editor .psst-key { font-size: 8pt; opacity: 0.7; }
)";

std::string buffer_text(GtkTextBuffer* buffer, const GtkTextIter* from,
                        const GtkTextIter* to) {
    gchar* raw = gtk_text_buffer_get_text(buffer, from, to, FALSE);
    std::string text = raw ? raw : "";
    g_free(raw);
    return text;
}

bool attaches_to_previous(char c) {
    return c != '\0' && std::strchr(" \t\n.,;:!?)]", c) != nullptr;
}
}

EditorWindow::Key EditorWindow::parse_key(const std::string& accel) {
    Key key;
    gtk_accelerator_parse(accel.c_str(), &key.keyval, &key.mods);
    if (key.keyval == 0)
        std::cerr << "[editor] Invalid key binding: " << accel << "\n";
    key.keyval = gdk_keyval_to_lower(key.keyval);
    return key;
}

bool EditorWindow::matches(const Key& key, const GdkEventKey* event) {
    if (key.keyval == 0) return false;
    guint keyval = gdk_keyval_to_lower(event->keyval);
    if (keyval == GDK_KEY_KP_Enter) keyval = GDK_KEY_Return;
    GdkModifierType mods = GdkModifierType(event->state & gtk_accelerator_get_default_mod_mask());
    return keyval == key.keyval && mods == key.mods;
}

std::string EditorWindow::label_of(const Key& key) {
    gchar* raw = gtk_accelerator_get_label(key.keyval, key.mods);
    std::string label = raw ? raw : "";
    g_free(raw);
    return label;
}

void EditorWindow::init(const Config& cfg, Actions actions) {
    cfg_ = cfg;
    actions_ = std::move(actions);
    pause_key_ = parse_key(cfg.editor_pause_key);
    type_key_  = parse_key(cfg.editor_type_key);
    copy_key_  = parse_key(cfg.editor_copy_key);
    correct_key_ = parse_key(cfg.editor_correct_key);
    undo_key_    = parse_key(cfg.editor_undo_key);

    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, kCss, -1, nullptr);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window_), "psst");
    gtk_window_set_default_size(GTK_WINDOW(window_), 720, 260);
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(window_), TRUE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window_), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(window_), TRUE);
    gtk_window_set_position(GTK_WINDOW(window_), GTK_WIN_POS_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(window_), "psst-editor");
    g_signal_connect(window_, "delete-event",
                     G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
                         auto* self = static_cast<EditorWindow*>(data);
                         if (self->actions_.cancel) self->actions_.cancel();
                         return TRUE;
                     }), this);
    g_signal_connect(window_, "key-press-event", G_CALLBACK(on_key), this);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_container_add(GTK_CONTAINER(window_), box);

    // Top row: recording state and VU meter.
    GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    // The icon is drawn, not a text glyph, so that every state icon has the
    // same size and sits centered on the label.
    icon_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(icon_, 14, 14);
    gtk_widget_set_valign(icon_, GTK_ALIGN_CENTER);
    g_signal_connect(icon_, "draw", G_CALLBACK(on_draw_icon), this);
    status_ = gtk_label_new(nullptr);
    gtk_label_set_width_chars(GTK_LABEL(status_), 10);
    gtk_label_set_xalign(GTK_LABEL(status_), 0.0f);
    gtk_widget_set_valign(status_, GTK_ALIGN_CENTER);
    level_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(level_, -1, 18);
    gtk_widget_set_valign(level_, GTK_ALIGN_CENTER);
    g_signal_connect(level_, "draw",
                     G_CALLBACK(+[](GtkWidget* widget, cairo_t* cr, gpointer data) -> gboolean {
                         auto* self = static_cast<EditorWindow*>(data);
                         draw_level_meter(cr, 0, 0,
                                          gtk_widget_get_allocated_width(widget),
                                          gtk_widget_get_allocated_height(widget),
                                          self->meter_.level());
                         return FALSE;
                     }), this);
    gtk_box_pack_start(GTK_BOX(top), icon_, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), status_, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), level_, TRUE, TRUE, 0);
    note_ = gtk_label_new(nullptr);
    gtk_label_set_width_chars(GTK_LABEL(note_), 28);
    gtk_label_set_max_width_chars(GTK_LABEL(note_), 28);
    gtk_label_set_ellipsize(GTK_LABEL(note_), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(note_), 1.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(note_), "psst-note");
    gtk_box_pack_start(GTK_BOX(top), note_, FALSE, FALSE, 0);
    GtkWidget* settings_btn = gtk_button_new_from_icon_name("preferences-system-symbolic",
                                                            GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(settings_btn, "Settings  (Ctrl+,)");
    gtk_widget_set_can_focus(settings_btn, FALSE);
    gtk_button_set_relief(GTK_BUTTON(settings_btn), GTK_RELIEF_NONE);
    g_signal_connect(settings_btn, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer data) {
                         auto* self = static_cast<EditorWindow*>(data);
                         if (self->actions_.settings) self->actions_.settings();
                     }), this);
    gtk_box_pack_start(GTK_BOX(top), settings_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);

    // Text field.
    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    // A wrapping text view reports its current width as its minimum width.
    // The AUTOMATIC policy keeps that minimum from reaching the window, which
    // would otherwise grow at each relayout; the wrapped text never needs a
    // horizontal scroll bar.
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    view_ = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view_), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view_), 6);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view_), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view_), 4);
    buffer_ = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_));
    // Start marks keep right gravity and end marks left gravity, so that text
    // inserted at the edges of a range stays outside of it.
    GtkTextIter origin;
    gtk_text_buffer_get_start_iter(buffer_, &origin);
    correct_from_ = gtk_text_buffer_create_mark(buffer_, nullptr, &origin, FALSE);
    correct_to_   = gtk_text_buffer_create_mark(buffer_, nullptr, &origin, TRUE);
    undo_from_    = gtk_text_buffer_create_mark(buffer_, nullptr, &origin, FALSE);
    undo_to_      = gtk_text_buffer_create_mark(buffer_, nullptr, &origin, TRUE);
    g_signal_connect_swapped(buffer_, "changed",
                             G_CALLBACK(+[](EditorWindow* self) { self->update_context(); }),
                             this);
    g_signal_connect(buffer_, "mark-set",
                     G_CALLBACK(+[](GtkTextBuffer* buffer, GtkTextIter*, GtkTextMark* mark,
                                    gpointer data) {
                         if (mark == gtk_text_buffer_get_insert(buffer))
                             static_cast<EditorWindow*>(data)->update_context();
                     }), this);
    gtk_container_add(GTK_CONTAINER(scroll), view_);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    // Buttons: the action name above its key binding.
    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(buttons), TRUE);
    auto add_button = [&](GCallback on_click, gpointer data) {
        GtkWidget* button = gtk_button_new();
        GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget* name = gtk_label_new(nullptr);
        GtkWidget* key = gtk_label_new(nullptr);
        gtk_style_context_add_class(gtk_widget_get_style_context(key), "psst-key");
        gtk_box_pack_start(GTK_BOX(labels), name, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(labels), key, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(button), labels);
        g_object_set_data(G_OBJECT(button), "psst-name", name);
        g_object_set_data(G_OBJECT(button), "psst-key", key);
        gtk_widget_set_can_focus(button, FALSE);
        g_signal_connect_swapped(button, "clicked", on_click, data);
        gtk_box_pack_start(GTK_BOX(buttons), button, TRUE, TRUE, 0);
        return button;
    };
    auto run_action = G_CALLBACK(+[](std::function<void()>* fn) { if (*fn) (*fn)(); });
    pause_btn_   = add_button(run_action, &actions_.pause);
    correct_btn_ = add_button(G_CALLBACK(+[](EditorWindow* self) { self->correct(); }), this);
    undo_btn_    = add_button(G_CALLBACK(+[](EditorWindow* self) { self->undo_correction(); }),
                              this);
    type_btn_    = add_button(run_action, &actions_.type);
    copy_btn_    = add_button(run_action, &actions_.copy);
    cancel_btn_  = add_button(run_action, &actions_.cancel);
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);

    gtk_widget_show_all(box);
    update_labels();
}

void EditorWindow::reconfigure(const Config& cfg) {
    cfg_ = cfg;
    pause_key_ = parse_key(cfg.editor_pause_key);
    type_key_  = parse_key(cfg.editor_type_key);
    copy_key_  = parse_key(cfg.editor_copy_key);
    correct_key_ = parse_key(cfg.editor_correct_key);
    undo_key_    = parse_key(cfg.editor_undo_key);
    update_labels();
}

unsigned long EditorWindow::xid() const {
#ifdef HAS_X11
    GdkWindow* gdk_win = window_ ? gtk_widget_get_window(window_) : nullptr;
    if (gdk_win && GDK_IS_X11_WINDOW(gdk_win)) return gdk_x11_window_get_xid(gdk_win);
#endif
    return 0;
}

void EditorWindow::update_labels() {
    set_button(pause_btn_, phase_ == Phase::Paused ? "Resume" : "Pause", pause_key_);
    set_button(correct_btn_, "Correct", correct_key_);
    set_button(undo_btn_, "Undo", undo_key_);
    set_button(type_btn_, "Type", type_key_);
    set_button(copy_btn_, "Copy", copy_key_);
    set_button(cancel_btn_, "Cancel", kEscape);
}

void EditorWindow::set_button(GtkWidget* button, const char* name, const Key& key) {
    auto* name_label = static_cast<GtkWidget*>(g_object_get_data(G_OBJECT(button), "psst-name"));
    auto* key_label  = static_cast<GtkWidget*>(g_object_get_data(G_OBJECT(button), "psst-key"));
    gtk_label_set_text(GTK_LABEL(name_label), name);
    gtk_label_set_text(GTK_LABEL(key_label), key.keyval ? label_of(key).c_str() : "");
}

void EditorWindow::set_note(const std::string& text) {
    gtk_label_set_text(GTK_LABEL(note_), text.c_str());
    gtk_widget_set_tooltip_text(note_, text.empty() ? nullptr : text.c_str());
}

void EditorWindow::show() {
    if (!window_) return;
    corrector_.cancel();
    gtk_text_buffer_set_text(buffer_, "", -1);
    undo_available_ = false;
    set_note("");
    gtk_widget_set_sensitive(correct_btn_, TRUE);
    gtk_widget_set_sensitive(undo_btn_, FALSE);
    meter_.reset();
    last_filled_ = -1;
    set_paused(false);
    gtk_widget_set_sensitive(view_, TRUE);
    gtk_widget_set_sensitive(pause_btn_, TRUE);
    gtk_widget_set_sensitive(type_btn_, TRUE);
    gtk_widget_set_sensitive(copy_btn_, TRUE);

    gtk_widget_show(window_);
    guint32 time = GDK_CURRENT_TIME;
#ifdef HAS_X11
    GdkWindow* gdk_win = gtk_widget_get_window(window_);
    if (gdk_win && GDK_IS_X11_WINDOW(gdk_win))
        time = gdk_x11_get_server_time(gdk_win);
#endif
    gtk_window_present_with_time(GTK_WINDOW(window_), time);
    gtk_widget_grab_focus(view_);
#ifdef HAS_X11
    // Window managers with focus-stealing prevention ignore the request above
    // for a window opened by a global hotkey. Ask for activation as a pager
    // does, which they honor.
    if (gdk_win && GDK_IS_X11_WINDOW(gdk_win)) {
        unsigned long xid = gdk_x11_window_get_xid(gdk_win);
        std::thread([xid]() { activate_window(xid); }).detach();
    }
#endif

    if (!timer_id_)
        timer_id_ = g_timeout_add(33, on_tick, this);
    std::cerr << "[editor] Shown\n";
}

void EditorWindow::hide() {
    if (!window_) return;
    corrector_.cancel();
    gtk_widget_hide(window_);
    if (timer_id_) { g_source_remove(timer_id_); timer_id_ = 0; }
    std::cerr << "[editor] Hidden\n";
}

bool EditorWindow::is_visible() const {
    return window_ && gtk_widget_get_visible(window_);
}

void EditorWindow::push_samples(const float* data, size_t count) {
    meter_.push(data, count);
}

void EditorWindow::set_paused(bool paused) {
    set_status(paused ? Phase::Paused : Phase::Recording);
    set_button(pause_btn_, paused ? "Resume" : "Pause", pause_key_);
}

void EditorWindow::set_finishing() {
    gtk_widget_set_sensitive(view_, FALSE);
    gtk_widget_set_sensitive(pause_btn_, FALSE);
    gtk_widget_set_sensitive(type_btn_, FALSE);
    gtk_widget_set_sensitive(copy_btn_, FALSE);
    gtk_widget_set_sensitive(correct_btn_, FALSE);
    gtk_widget_set_sensitive(undo_btn_, FALSE);
    corrector_.cancel();
    set_note("");
    set_status(Phase::Finishing);
}

void EditorWindow::set_status(Phase phase) {
    phase_ = phase;
    const char* text = phase == Phase::Recording ? "REC"
                     : phase == Phase::Paused    ? "PAUSED" : "finishing…";
    const char* css  = phase == Phase::Recording ? "psst-rec"
                     : phase == Phase::Paused    ? "psst-paused" : "psst-status";
    GtkStyleContext* ctx = gtk_widget_get_style_context(status_);
    for (const char* c : {"psst-rec", "psst-paused", "psst-status"})
        gtk_style_context_remove_class(ctx, c);
    gtk_style_context_add_class(ctx, css);
    gtk_label_set_text(GTK_LABEL(status_), text);
    gtk_widget_queue_draw(icon_);
}

gboolean EditorWindow::on_draw_icon(GtkWidget* widget, cairo_t* cr, gpointer data) {
    auto* self = static_cast<EditorWindow*>(data);
    double w = gtk_widget_get_allocated_width(widget);
    double h = gtk_widget_get_allocated_height(widget);
    double size = std::min(w, h);
    double x = (w - size) / 2.0, y = (h - size) / 2.0;
    switch (self->phase_) {
    case Phase::Recording:  // red dot
        cairo_set_source_rgb(cr, 1.0, 0.27, 0.27);
        cairo_arc(cr, w / 2.0, h / 2.0, size * 0.4, 0, 2 * G_PI);
        cairo_fill(cr);
        break;
    case Phase::Paused:     // two orange bars
        cairo_set_source_rgb(cr, 1.0, 0.69, 0.13);
        cairo_rectangle(cr, x + size * 0.15, y + size * 0.1, size * 0.25, size * 0.8);
        cairo_rectangle(cr, x + size * 0.6, y + size * 0.1, size * 0.25, size * 0.8);
        cairo_fill(cr);
        break;
    case Phase::Finishing:  // gray ring
        cairo_set_source_rgb(cr, 0.54, 0.54, 0.6);
        cairo_set_line_width(cr, 2.0);
        cairo_arc(cr, w / 2.0, h / 2.0, size * 0.35, 0, 2 * G_PI);
        cairo_stroke(cr);
        break;
    }
    return FALSE;
}

void EditorWindow::insert_transcript(const std::string& transcript) {
    GtkTextIter start, cursor, end;
    // Dictation replaces a selection, as typing does.
    gtk_text_buffer_delete_selection(buffer_, TRUE, TRUE);
    gtk_text_buffer_get_bounds(buffer_, &start, &end);
    gtk_text_buffer_get_iter_at_mark(buffer_, &cursor, gtk_text_buffer_get_insert(buffer_));

    std::string before = buffer_text(buffer_, &start, &cursor);
    std::string after  = buffer_text(buffer_, &cursor, &end);
    // The text buffer accepts valid UTF-8 only.
    gchar* valid = g_utf8_make_valid(transcript.c_str(), -1);
    std::string next = append_transcript(before, valid, cfg_.punctuation_words,
                                         cfg_.punctuation_enabled);
    g_free(valid);
    if (!after.empty() && !next.empty() && !attaches_to_previous(after[0]) &&
        next.back() != ' ' && next.back() != '\n')
        next += ' ';

    // Replace only what changed after the common prefix of the old and new text.
    size_t common = 0;
    while (common < before.size() && common < next.size() && before[common] == next[common])
        ++common;
    while (common > 0 && common < before.size() &&
           (static_cast<unsigned char>(before[common]) & 0xC0) == 0x80)
        --common;
    glong common_chars = g_utf8_strlen(before.data(), static_cast<gssize>(common));

    gtk_text_buffer_begin_user_action(buffer_);
    GtkTextIter from;
    gtk_text_buffer_get_iter_at_offset(buffer_, &from, static_cast<gint>(common_chars));
    gtk_text_buffer_get_iter_at_mark(buffer_, &cursor, gtk_text_buffer_get_insert(buffer_));
    gtk_text_buffer_delete(buffer_, &from, &cursor);
    std::string added = next.substr(common);
    gtk_text_buffer_insert(buffer_, &from, added.data(), static_cast<gint>(added.size()));
    gtk_text_buffer_place_cursor(buffer_, &from);
    gtk_text_buffer_end_user_action(buffer_);

    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(view_),
                                       gtk_text_buffer_get_insert(buffer_));
}

std::string EditorWindow::text() const {
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer_, &start, &end);
    return buffer_text(buffer_, &start, &end);
}

std::string EditorWindow::context() const {
    std::lock_guard<std::mutex> lk(context_mtx_);
    return context_;
}

void EditorWindow::update_context() {
    GtkTextIter start, cursor;
    gtk_text_buffer_get_start_iter(buffer_, &start);
    gtk_text_buffer_get_iter_at_mark(buffer_, &cursor, gtk_text_buffer_get_insert(buffer_));
    std::string text = buffer_text(buffer_, &start, &cursor);
    std::lock_guard<std::mutex> lk(context_mtx_);
    context_ = std::move(text);
}

std::string EditorWindow::text_between(GtkTextMark* from, GtkTextMark* to) const {
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_mark(buffer_, &a, from);
    gtk_text_buffer_get_iter_at_mark(buffer_, &b, to);
    return buffer_text(buffer_, &a, &b);
}

std::pair<int, int> EditorWindow::replace_range(GtkTextIter* from, GtkTextIter* to,
                                                const std::string& text) {
    int start = gtk_text_iter_get_offset(from);
    gtk_text_buffer_begin_user_action(buffer_);
    gtk_text_buffer_delete(buffer_, from, to);
    gtk_text_buffer_insert(buffer_, from, text.data(), static_cast<gint>(text.size()));
    gtk_text_buffer_end_user_action(buffer_);
    return {start, gtk_text_iter_get_offset(from)};
}

void EditorWindow::correct() {
    if (corrector_.busy()) return;

    GtkTextIter from, to;
    if (!gtk_text_buffer_get_selection_bounds(buffer_, &from, &to))
        gtk_text_buffer_get_bounds(buffer_, &from, &to);
    std::string text = buffer_text(buffer_, &from, &to);
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
        set_note("Nothing to correct.");
        return;
    }

    gtk_text_buffer_move_mark(buffer_, correct_from_, &from);
    gtk_text_buffer_move_mark(buffer_, correct_to_, &to);
    correct_original_ = text;
    gtk_widget_set_sensitive(correct_btn_, FALSE);
    set_note("Correcting…");
    corrector_.run(cfg_, text, [this](bool ok, const std::string& result) {
        on_corrected(ok, result);
    });
}

void EditorWindow::on_corrected(bool ok, const std::string& result) {
    gtk_widget_set_sensitive(correct_btn_, phase_ != Phase::Finishing);
    if (!ok) {
        set_note("Correction failed: " + result);
        return;
    }
    // Dictation or typing inside the range during the correction wins.
    if (text_between(correct_from_, correct_to_) != correct_original_) {
        set_note("Text changed during the correction; result discarded.");
        return;
    }
    gchar* valid = g_utf8_make_valid(result.c_str(), -1);
    std::string corrected = valid;
    g_free(valid);
    if (corrected == correct_original_) {
        set_note("No corrections.");
        return;
    }

    GtkTextIter from, to;
    gtk_text_buffer_get_iter_at_mark(buffer_, &from, correct_from_);
    gtk_text_buffer_get_iter_at_mark(buffer_, &to, correct_to_);
    auto [start, end] = replace_range(&from, &to, corrected);
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(buffer_, &a, start);
    gtk_text_buffer_get_iter_at_offset(buffer_, &b, end);
    gtk_text_buffer_move_mark(buffer_, undo_from_, &a);
    gtk_text_buffer_move_mark(buffer_, undo_to_, &b);
    undo_original_ = correct_original_;
    undo_corrected_ = corrected;
    undo_available_ = true;
    gtk_widget_set_sensitive(undo_btn_, phase_ != Phase::Finishing);
    set_note("Corrected.");
}

void EditorWindow::undo_correction() {
    if (!undo_available_) return;
    undo_available_ = false;
    gtk_widget_set_sensitive(undo_btn_, FALSE);
    if (text_between(undo_from_, undo_to_) != undo_corrected_) {
        set_note("Text changed after the correction; cannot undo.");
        return;
    }
    GtkTextIter from, to;
    gtk_text_buffer_get_iter_at_mark(buffer_, &from, undo_from_);
    gtk_text_buffer_get_iter_at_mark(buffer_, &to, undo_to_);
    replace_range(&from, &to, undo_original_);
    set_note("Correction undone.");
}

gboolean EditorWindow::on_key(GtkWidget*, GdkEventKey* event, gpointer data) {
    auto* self = static_cast<EditorWindow*>(data);
    const std::function<void()>* action = nullptr;
    GdkModifierType mods = GdkModifierType(event->state & gtk_accelerator_get_default_mod_mask());
    if (event->keyval == GDK_KEY_Escape)       action = &self->actions_.cancel;
    else if (event->keyval == GDK_KEY_comma && mods == GDK_CONTROL_MASK)
        action = &self->actions_.settings;
    else if (matches(self->pause_key_, event)) action = &self->actions_.pause;
    else if (matches(self->type_key_, event))  action = &self->actions_.type;
    else if (matches(self->copy_key_, event))  action = &self->actions_.copy;
    else if (matches(self->correct_key_, event)) {
        if (gtk_widget_get_sensitive(self->correct_btn_)) self->correct();
        return TRUE;
    } else if (matches(self->undo_key_, event) && self->undo_available_) {
        self->undo_correction();
        return TRUE;
    }
    if (!action) return FALSE;  // the text field handles the key
    if (*action) (*action)();
    return TRUE;
}

gboolean EditorWindow::on_tick(gpointer data) {
    auto* self = static_cast<EditorWindow*>(data);
    int filled = static_cast<int>(self->meter_.level() * (float)kMeterSegments);
    if (filled != self->last_filled_) {
        self->last_filled_ = filled;
        gtk_widget_queue_draw(self->level_);
    }
    return TRUE;
}
