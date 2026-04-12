#pragma once
#include "config.h"
#include <functional>

class TrayIcon {
public:
    enum class State { Idle, Recording, Transcribing };

    bool init(const Config& cfg, const std::string& icon_dir);
    void set_state(State state);

    using QuitCallback = std::function<void()>;
    void set_quit_callback(QuitCallback cb) { quit_cb_ = std::move(cb); }

    void shutdown();

private:
    QuitCallback quit_cb_;
    struct Impl;
    Impl* impl_ = nullptr;
};
