#include "overlay.h"
#include <cmath>
#include <algorithm>
#include <iostream>

#ifdef HAS_X11
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <gdk/gdkx.h>
#endif

void OverlayWindow::init(const Config& cfg) {
    cfg_ = cfg;

    window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window_), "psst");
    gtk_window_set_default_size(GTK_WINDOW(window_), 320, 52);
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(window_), TRUE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window_), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(window_), TRUE);
    gtk_window_set_position(GTK_WINDOW(window_), GTK_WIN_POS_CENTER);
    gtk_window_set_type_hint(GTK_WINDOW(window_), GDK_WINDOW_TYPE_HINT_NOTIFICATION);
    gtk_window_set_accept_focus(GTK_WINDOW(window_), FALSE);
    gtk_window_set_focus_on_map(GTK_WINDOW(window_), FALSE);

    GdkScreen* screen = gtk_widget_get_screen(window_);
    GdkVisual* visual = gdk_screen_get_rgba_visual(screen);
    if (visual) gtk_widget_set_visual(window_, visual);
    gtk_widget_set_app_paintable(window_, TRUE);

    drawing_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(drawing_, 320, 52);
    gtk_widget_set_can_focus(drawing_, FALSE);
    g_signal_connect(drawing_, "draw", G_CALLBACK(on_draw), this);
    gtk_container_add(GTK_CONTAINER(window_), drawing_);
    g_signal_connect(window_, "delete-event",
                     G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer) -> gboolean {
                         return TRUE;
                     }), nullptr);

    gtk_widget_show_all(window_);
    gtk_widget_hide(window_);
}

void OverlayWindow::show() {
    if (!window_) return;
    level_.store(0.0f);
    last_filled_ = -1;
    gtk_widget_show(window_);
    grab_esc();
    if (!timer_id_)
        timer_id_ = g_timeout_add(33, on_tick, this);  // ~30fps
    std::cerr << "[overlay] Shown\n";
}

void OverlayWindow::hide() {
    if (!window_) return;
    ungrab_esc();
    gtk_widget_hide(window_);
    if (timer_id_) { g_source_remove(timer_id_); timer_id_ = 0; }
    std::cerr << "[overlay] Hidden\n";
}

void OverlayWindow::grab_esc() {
#ifdef HAS_X11
    GdkDisplay* gdk_disp = gdk_display_get_default();
    Display* dpy = gdk_x11_display_get_xdisplay(gdk_disp);
    Window root = DefaultRootWindow(dpy);
    KeyCode esc = XKeysymToKeycode(dpy, XK_Escape);
    unsigned int lock_masks[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    for (auto lm : lock_masks)
        XGrabKey(dpy, (int)esc, lm, root, False, GrabModeAsync, GrabModeAsync);
    XFlush(dpy);
    gdk_window_add_filter(nullptr, esc_x11_filter, this);
#endif
}

void OverlayWindow::ungrab_esc() {
#ifdef HAS_X11
    GdkDisplay* gdk_disp = gdk_display_get_default();
    Display* dpy = gdk_x11_display_get_xdisplay(gdk_disp);
    Window root = DefaultRootWindow(dpy);
    KeyCode esc = XKeysymToKeycode(dpy, XK_Escape);
    unsigned int lock_masks[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    for (auto lm : lock_masks)
        XUngrabKey(dpy, (int)esc, lm, root);
    XFlush(dpy);
    gdk_window_remove_filter(nullptr, esc_x11_filter, this);
#endif
}

bool OverlayWindow::is_visible() const {
    return window_ && gtk_widget_get_visible(window_);
}

void OverlayWindow::push_samples(const float* data, size_t count) {
    float sum = 0.0f;
    for (size_t i = 0; i < count; ++i)
        sum += data[i] * data[i];
    float rms = sqrtf(sum / (float)count);
    float lvl = std::min(1.0f, rms * 3.0f);
    level_.store(lvl);
}

gboolean OverlayWindow::on_draw(GtkWidget* widget, cairo_t* cr, gpointer data) {
    auto* self = static_cast<OverlayWindow*>(data);

    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);
    float lvl = self->level_.load();

    // Dark background
    cairo_set_source_rgba(cr, 0.12, 0.12, 0.15, 0.88);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    double pad = 10.0;

    // Red dot + "REC"
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 14.0);
    cairo_set_source_rgb(cr, 1.0, 0.27, 0.27);
    cairo_move_to(cr, pad + 10, h / 2.0 + 5);
    cairo_show_text(cr, "\xe2\x97\x8f REC");

    // Meter: 20 colored segments
    double mx = pad + 75, my = pad + 6, mw = (double)w - mx - pad - 10, mh = (double)h - 2.0 * pad - 12;
    int bars = 20;
    int filled = (int)(lvl * (float)bars);
    if (filled > bars) filled = bars;
    double seg_w = mw / (double)bars;
    double gap = 2.0;

    for (int i = 0; i < bars; ++i) {
        double sx = mx + (double)i * seg_w;
        if (i < filled) {
            float t = (float)i / (float)bars;
            double r = std::min(1.0, (double)t * 2.5);
            double g = std::min(1.0, 2.0 * (1.0 - (double)t));
            cairo_set_source_rgb(cr, r, g, 0.15);
        } else {
            cairo_set_source_rgba(cr, 0.25, 0.25, 0.3, 0.6);
        }
        cairo_rectangle(cr, sx, my, seg_w - gap, mh);
        cairo_fill(cr);
    }

    // "ESC" hint at bottom-right
    cairo_set_font_size(cr, 10.0);
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.7);
    cairo_move_to(cr, (double)w - pad - 70, h - pad - 3);
    cairo_show_text(cr, "ESC cancel");

    // Border
    double border = 3.0;
    cairo_set_source_rgb(cr, 0.0 / 255.0, 114.0 / 255.0, 163.0 / 255.0);  // #0072a3
    cairo_set_line_width(cr, border);
    cairo_rectangle(cr, border / 2.0, border / 2.0, w - border, h - border);
    cairo_stroke(cr);

    return FALSE;
}

gboolean OverlayWindow::on_tick(gpointer data) {
    auto* self = static_cast<OverlayWindow*>(data);

    float lvl = self->level_.load();
    int filled = (int)(lvl * 20.0f);

    if (filled != self->last_filled_) {
        self->last_filled_ = filled;
        if (self->drawing_)
            gtk_widget_queue_draw(self->drawing_);
    }
    return TRUE;
}

#ifdef HAS_X11
GdkFilterReturn OverlayWindow::esc_x11_filter(GdkXEvent* xevent, GdkEvent*, gpointer data) {
    XEvent* ev = static_cast<XEvent*>(xevent);
    if (ev->type == KeyPress) {
        Display* dpy = ev->xkey.display;
        KeyCode esc = XKeysymToKeycode(dpy, XK_Escape);
        if (ev->xkey.keycode == esc) {
            auto* self = static_cast<OverlayWindow*>(data);
            std::cerr << "[overlay] ESC pressed (global)\n";
            if (self->esc_cb_) self->esc_cb_();
            return GDK_FILTER_REMOVE;
        }
    }
    return GDK_FILTER_CONTINUE;
}
#endif
