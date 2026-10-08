#include "correct.h"
#include <gio/gio.h>
#include <iostream>
#include <vector>

namespace {
constexpr guint kTimeoutSeconds = 90;
}

struct Corrector::Job {
    Corrector* owner = nullptr;  // null after cancel
    Done done;
    GCancellable* cancellable = nullptr;
    GSubprocess* process = nullptr;
    guint timeout_id = 0;
    bool timed_out = false;
};

void Corrector::run(const Config& cfg, const std::string& text, Done done) {
    cancel();

    std::vector<std::string> args = {cfg.correction_command, "-p"};
    if (!cfg.correction_model.empty()) {
        args.push_back("--model");
        args.push_back(cfg.correction_model);
    }
    for (const char* a : {"--tools", "", "--no-session-persistence",
                          "--strict-mcp-config", "--setting-sources", ""})
        args.push_back(a);
    args.push_back("--system-prompt");
    args.push_back(cfg.correction_prompt);

    std::vector<const gchar*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    GError* error = nullptr;
    GSubprocess* process = g_subprocess_newv(
        argv.data(),
        GSubprocessFlags(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                         G_SUBPROCESS_FLAGS_STDERR_PIPE),
        &error);
    if (!process) {
        std::string message = std::string("Cannot run ") + cfg.correction_command + ": " +
                              (error ? error->message : "unknown error");
        g_clear_error(&error);
        std::cerr << "[correct] " << message << "\n";
        if (done) done(false, message);
        return;
    }

    auto* job = new Job;
    job->owner = this;
    job->done = std::move(done);
    job->cancellable = g_cancellable_new();
    job->process = process;
    job->timeout_id = g_timeout_add_seconds(kTimeoutSeconds, +[](gpointer data) -> gboolean {
        auto* j = static_cast<Job*>(data);
        j->timeout_id = 0;
        j->timed_out = true;
        g_subprocess_force_exit(j->process);
        return G_SOURCE_REMOVE;
    }, job);
    cancellable_ = job->cancellable;
    job_ = job;

    std::cerr << "[correct] Correcting " << text.size() << " chars with "
              << cfg.correction_command << " (" << cfg.correction_model << ")\n";

    g_subprocess_communicate_utf8_async(
        process, text.c_str(), job->cancellable,
        +[](GObject* source, GAsyncResult* res, gpointer data) {
            auto* j = static_cast<Job*>(data);
            gchar* out = nullptr;
            gchar* err = nullptr;
            GError* error = nullptr;
            bool finished = g_subprocess_communicate_utf8_finish(
                G_SUBPROCESS(source), res, &out, &err, &error);

            bool ok = false;
            std::string result;
            if (!finished) {
                result = error ? error->message : "unknown error";
            } else if (j->timed_out) {
                result = "The correction took too long.";
            } else if (!g_subprocess_get_successful(j->process)) {
                result = err && *err ? g_strstrip(err) : (out && *out ? g_strstrip(out)
                                                                        : "The command failed.");
            } else {
                result = out ? out : "";
                while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
                    result.pop_back();
                ok = !result.empty();
                if (!ok) result = "The correction is empty.";
            }
            g_clear_error(&error);
            g_free(out);
            g_free(err);
            finish(j, ok, result);
        }, job);
}

void Corrector::finish(Job* job, bool ok, const std::string& result) {
    if (job->timeout_id) g_source_remove(job->timeout_id);
    Corrector* owner = job->owner;
    if (owner) {
        owner->cancellable_ = nullptr;
        owner->job_ = nullptr;
        if (!ok) std::cerr << "[correct] Failed: " << result << "\n";
        else     std::cerr << "[correct] Done (" << result.size() << " chars)\n";
        if (job->done) job->done(ok, result);
    }
    g_object_unref(job->cancellable);
    g_object_unref(job->process);
    delete job;
}

void Corrector::cancel() {
    if (!job_) return;
    job_->owner = nullptr;
    g_cancellable_cancel(job_->cancellable);
    g_subprocess_force_exit(job_->process);
    cancellable_ = nullptr;
    job_ = nullptr;
}
