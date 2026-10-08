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

    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, kCss, -1, nullptr);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window_), "psst");
    gtk_window_set_default_size(GTK_WINDOW(window_), 600, 240);
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
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    view_ = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view_), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view_), 6);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view_), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view_), 4);
    buffer_ = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_));
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

    // Buttons, labelled with their key bindings.
    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(buttons), TRUE);
    auto add_button = [&](const std::function<void()>* action) {
        GtkWidget* button = gtk_button_new();
        gtk_widget_set_can_focus(button, FALSE);
        g_signal_connect(button, "clicked",
                         G_CALLBACK(+[](GtkButton*, gpointer data) {
                             auto* fn = static_cast<const std::function<void()>*>(data);
                             if (*fn) (*fn)();
                         }), const_cast<std::function<void()>*>(action));
        return button;
    };
    pause_btn_  = add_button(&actions_.pause);
    type_btn_   = add_button(&actions_.type);
    copy_btn_   = add_button(&actions_.copy);
    cancel_btn_ = add_button(&actions_.cancel);
    gtk_box_pack_start(GTK_BOX(buttons), pause_btn_, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), type_btn_, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), copy_btn_, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), cancel_btn_, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);

    gtk_widget_show_all(box);
    update_labels();
}

void EditorWindow::reconfigure(const Config& cfg) {
    cfg_ = cfg;
    pause_key_ = parse_key(cfg.editor_pause_key);
    type_key_  = parse_key(cfg.editor_type_key);
    copy_key_  = parse_key(cfg.editor_copy_key);
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
    auto with_key = [](const char* name, const Key& key) {
        std::string label = name;
        if (key.keyval) label += "  (" + label_of(key) + ")";
        return label;
    };
    gtk_button_set_label(GTK_BUTTON(type_btn_), with_key("Type", type_key_).c_str());
    gtk_button_set_label(GTK_BUTTON(copy_btn_), with_key("Copy", copy_key_).c_str());
    gtk_button_set_label(GTK_BUTTON(cancel_btn_),
                         with_key("Cancel", Key{GDK_KEY_Escape, GdkModifierType(0)}).c_str());

    // Size the pause button for its longer label, so that the buttons keep
    // their width when the label changes.
    gtk_widget_set_size_request(pause_btn_, -1, -1);
    gtk_button_set_label(GTK_BUTTON(pause_btn_), pause_label(true).c_str());
    gint width = 0;
    gtk_widget_get_preferred_width(pause_btn_, nullptr, &width);
    gtk_widget_set_size_request(pause_btn_, width, -1);
    gtk_button_set_label(GTK_BUTTON(pause_btn_),
                         pause_label(phase_ == Phase::Paused).c_str());
}

std::string EditorWindow::pause_label(bool paused) const {
    std::string label = paused ? "Resume" : "Pause";
    if (pause_key_.keyval) label += "  (" + label_of(pause_key_) + ")";
    return label;
}

void EditorWindow::show() {
    if (!window_) return;
    gtk_text_buffer_set_text(buffer_, "", -1);
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
    gtk_button_set_label(GTK_BUTTON(pause_btn_), pause_label(paused).c_str());
    set_status(paused ? Phase::Paused : Phase::Recording);
}

void EditorWindow::set_finishing() {
    gtk_widget_set_sensitive(view_, FALSE);
    gtk_widget_set_sensitive(pause_btn_, FALSE);
    gtk_widget_set_sensitive(type_btn_, FALSE);
    gtk_widget_set_sensitive(copy_btn_, FALSE);
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
