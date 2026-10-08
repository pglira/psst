#pragma once
#include "config.h"
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <atomic>
#include <functional>

// Level of an audio stream for a VU meter, in [0, 1]. It auto-scales to a dB
// window below a decaying peak, so the meter fills regardless of mic gain.
class LevelMeter {
public:
    void reset();

    // Add a chunk of samples. Call from one thread only.
    void push(const float* data, size_t count);

    float level() const { return level_.load(); }

private:
    // Below this dBFS the meter reads empty; also the auto-scale window's
    // lowest possible top, so a silent pause can't fill the bar with noise.
    static constexpr float kNoiseFloorDb = -25.0f;

    std::atomic<float> level_{0.0f};
    float peak_db_ = kNoiseFloorDb;  // decaying peak for auto-scaling
};

// Number of segments of the VU meter.
constexpr int kMeterSegments = 20;

// Draw a VU meter of `level` in [0, 1] as segments from green to red.
void draw_level_meter(cairo_t* cr, double x, double y, double w, double h, float level);

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

    LevelMeter meter_;
    int last_filled_ = -1;           // cache to skip redundant redraws

    EscCallback esc_cb_;
};
