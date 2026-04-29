#include "sound.h"

#include <pulse/simple.h>
#include <pulse/error.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

namespace {

constexpr int    kRate     = 44100;
constexpr float  kPi       = 3.14159265358979323846f;

std::atomic<bool> g_enabled{true};

// Synthesize a short two-tone chirp reminiscent of a Star Trek communicator:
// a logarithmic pitch glide between f0 and f1 with a layered fifth above,
// shaped by a quick attack and exponential decay.
std::vector<int16_t> synth_chirp(float f0, float f1, float duration_s) {
    const size_t n = (size_t)(kRate * duration_s);
    std::vector<int16_t> out(n);

    float phase_a = 0.0f;
    float phase_b = 0.0f;

    for (size_t i = 0; i < n; ++i) {
        float t = (float)i / (float)n;                    // 0..1
        // Logarithmic pitch glide feels more musical than linear.
        float f = f0 * std::pow(f1 / f0, t);
        float f_fifth = f * 1.5f;                          // perfect fifth above

        phase_a += 2.0f * kPi * f       / kRate;
        phase_b += 2.0f * kPi * f_fifth / kRate;

        // Envelope: 8 ms attack, exponential decay.
        float attack_s = 0.008f;
        float attack   = std::min(1.0f, (i / (float)kRate) / attack_s);
        float decay    = std::exp(-3.5f * t);
        float env      = attack * decay;

        float s = 0.55f * std::sin(phase_a) + 0.30f * std::sin(phase_b);
        float v = s * env * 0.85f;                         // headroom
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        out[i] = (int16_t)(v * 32767.0f);
    }
    return out;
}

void play_pcm_async(std::vector<int16_t> pcm) {
    std::thread([pcm = std::move(pcm)]() mutable {
        pa_sample_spec ss{};
        ss.format   = PA_SAMPLE_S16LE;
        ss.channels = 1;
        ss.rate     = kRate;

        int err = 0;
        pa_simple* s = pa_simple_new(
            nullptr, "psst", PA_STREAM_PLAYBACK, nullptr,
            "notification", &ss, nullptr, nullptr, &err);
        if (!s) {
            std::cerr << "[sound] pa_simple_new failed: "
                      << pa_strerror(err) << "\n";
            return;
        }
        if (pa_simple_write(s, pcm.data(), pcm.size() * sizeof(int16_t), &err) < 0)
            std::cerr << "[sound] write failed: " << pa_strerror(err) << "\n";
        pa_simple_drain(s, &err);
        pa_simple_free(s);
    }).detach();
}

} // namespace

void sound_set_enabled(bool enabled) { g_enabled.store(enabled); }

void sound_play_activate() {
    if (!g_enabled.load()) return;
    // Rising chirp: A5 → E6, 110 ms.
    play_pcm_async(synth_chirp(880.0f, 1320.0f, 0.11f));
}

void sound_play_deactivate() {
    if (!g_enabled.load()) return;
    // Falling chirp: E6 → A5, 110 ms.
    play_pcm_async(synth_chirp(1320.0f, 880.0f, 0.11f));
}
