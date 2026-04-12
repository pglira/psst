#pragma once
#include "config.h"
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <atomic>
#include <functional>

class OverlayWindow {
public:
    void init(const Config& cfg);
    void show();
    void hide();
    bool is_visible() const;

    void push_samples(const float* data, size_t count);

    using EscCallback = std::function<void()>;
    void set_esc_callback(EscCallback cb) { esc_cb_ = std::move(cb); }

private:
    static gboolean on_draw(GtkWidget* widget, cairo_t* cr, gpointer data);
    static gboolean on_tick(gpointer data);

    void grab_esc();
    void ungrab_esc();
#ifdef HAS_X11
    static GdkFilterReturn esc_x11_filter(GdkXEvent* xevent, GdkEvent* event, gpointer data);
#endif

    Config cfg_;
    GtkWidget* window_  = nullptr;
    GtkWidget* drawing_ = nullptr;
    guint timer_id_ = 0;

    std::atomic<float> level_{0.0f};
    int last_filled_ = -1;  // cache to skip redundant redraws

    EscCallback esc_cb_;
};
