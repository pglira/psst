#include "meter.h"
#include <algorithm>
#include <cmath>

void draw_level_meter(cairo_t* cr, double x, double y, double w, double h, float level) {
    int filled = std::min(kMeterSegments, (int)(level * (float)kMeterSegments));
    double seg_w = w / (double)kMeterSegments;
    double gap = 2.0;

    for (int i = 0; i < kMeterSegments; ++i) {
        double sx = x + (double)i * seg_w;
        if (i < filled) {
            float t = (float)i / (float)kMeterSegments;
            double r = std::min(1.0, (double)t * 2.5);
            double g = std::min(1.0, 2.0 * (1.0 - (double)t));
            cairo_set_source_rgb(cr, r, g, 0.15);
        } else {
            cairo_set_source_rgba(cr, 0.25, 0.25, 0.3, 0.6);
        }
        cairo_rectangle(cr, sx, y, seg_w - gap, h);
        cairo_fill(cr);
    }
}

void LevelMeter::reset() {
    level_.store(0.0f);
    peak_db_ = kNoiseFloorDb;
}

void LevelMeter::push(const float* data, size_t count) {
    if (count == 0) return;

    float sum = 0.0f;
    for (size_t i = 0; i < count; ++i)
        sum += data[i] * data[i];
    float rms = sqrtf(sum / (float)count);

    float db = 20.0f * log10f(rms + 1e-9f);  // dBFS; +eps avoids log(0)

    // Auto-scale to a dB window below a decaying peak so the meter fills
    // regardless of mic gain; kNoiseFloorDb keeps a silent pause empty.
    peak_db_ = std::max(db, peak_db_ - 0.5f);  // 0.5 dB/chunk = ~10 dB/s at 20 chunks/s
    float top_db = std::max(peak_db_, kNoiseFloorDb);
    constexpr float window_db = 30.0f;
    float lvl = std::clamp((db - (top_db - window_db)) / window_db, 0.0f, 1.0f);

    // Fast attack, slow decay so the bar tracks peaks but eases down.
    float prev = level_.load();
    if (lvl < prev)
        lvl = prev * 0.7f + lvl * 0.3f;
    level_.store(lvl);
}

