#pragma once
#include "config.h"
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Splits a growing recording into utterances at pauses in speech.
//
// Speech detection compares the RMS level of 20 ms frames with an adaptive
// noise floor. An utterance ends after `pause_ms` of silence, or at the
// quietest frame of its second half when it reaches `max_ms`.
class UtteranceSplitter {
public:
    using Range = std::pair<size_t, size_t>;  // [begin, end) sample indices

    UtteranceSplitter(int sample_rate, int pause_ms, int max_ms);

    // Analyse the next `count` samples of the recording. Returns the
    // utterances that end in them.
    std::vector<Range> feed(const float* data, size_t count);

    // End of the recording: return the utterance in progress, if any.
    std::vector<Range> flush();

private:
    void on_frame(float rms, std::vector<Range>& out);
    void emit(size_t end, std::vector<Range>& out);

    size_t frame_len_;
    size_t pause_frames_;
    size_t max_frames_;
    size_t min_speech_frames_;
    size_t pad_;

    std::vector<float> partial_;     // samples of the incomplete frame
    size_t frames_ = 0;              // frames analysed so far
    float  noise_  = -1.0f;          // noise floor (RMS), < 0 until known
    size_t emitted_end_ = 0;         // end sample of the previous utterance

    bool   in_speech_ = false;
    bool   after_cut_ = false;       // the utterance continues a cut one
    size_t speech_begin_ = 0;        // first speech frame of the utterance
    size_t speech_last_ = 0;         // last speech frame of the utterance
    size_t speech_frames_ = 0;       // speech frames in the utterance
    std::vector<float> levels_;      // frame RMS since speech_begin_
    std::vector<bool>  voiced_;      // speech flag per frame since speech_begin_
};

// Runs live dictation for one recording on a background thread.
//
// The thread polls the recording, cuts it into utterances, and passes each
// utterance to the handler in order. Sessions run one after another: a new
// session waits on its own thread until the previous session ends.
class LiveDictation {
public:
    // Return the samples of the recording from index `from` to its end.
    using Fetch = std::function<std::vector<float>(size_t from)>;
    // Process one utterance; `cancelled` turns true when the session cancels.
    using Handler = std::function<void(const std::vector<float>& utterance,
                                       const std::atomic<bool>& cancelled)>;
    // Called once when the session ends.
    using Done = std::function<void(bool cancelled)>;

    // Start a session. Returns at once.
    void start(const Config& cfg, Fetch fetch, Handler handler, Done done);

    // The recording stopped with `samples` as its complete audio. Process the
    // remaining utterances, then end the session.
    void finish(std::vector<float> samples);

    // End the current session without processing more utterances.
    void cancel();

    // Wait for all sessions to end.
    void join();

    ~LiveDictation() { cancel(); join(); }

private:
    struct Session {
        std::mutex mtx;
        bool finished = false;
        std::vector<float> final_samples;
        std::atomic<bool> cancelled{false};
    };

    static void run(std::thread previous, std::shared_ptr<Session> session,
                    Config cfg, Fetch fetch, Handler handler, Done done);

    std::thread thread_;
    std::shared_ptr<Session> session_;
};
