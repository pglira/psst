#pragma once
#include "config.h"
#include <functional>
#include <string>

typedef struct _GCancellable GCancellable;

// Corrects a text with an LLM through the claude CLI ("claude -p"). The text
// goes to the CLI on stdin; the corrected text comes back on stdout. The
// CLI runs without tools, MCP servers and settings files.
//
// Runs on the GTK main loop: run() returns at once, and `done` is called on
// the main loop later.
class Corrector {
public:
    // ok: `result` is the corrected text; otherwise an error message.
    using Done = std::function<void(bool ok, const std::string& result)>;

    // Start a correction. A correction that still runs is cancelled; its
    // `done` is not called.
    void run(const Config& cfg, const std::string& text, Done done);

    // Cancel the running correction; its `done` is not called.
    void cancel();

    bool busy() const { return cancellable_ != nullptr; }

    ~Corrector() { cancel(); }

private:
    struct Job;
    static void finish(Job* job, bool ok, const std::string& result);

    GCancellable* cancellable_ = nullptr;
    Job* job_ = nullptr;
};
