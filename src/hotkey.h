#pragma once
#include "config.h"
#include <functional>
#include <atomic>

class HotkeyListener {
public:
    using Callback = std::function<void()>;

    bool init(const Config& cfg, Callback on_toggle);

    // Start listening for the hotkey in a background thread.
    void start();

    // Stop listening.
    void stop();

    // Returns "x11" or "wayland" based on detected session.
    static std::string detect_session();

    ~HotkeyListener();

private:
    void listen_x11();
    void parse_binding(const std::string& bind);

    Config cfg_;
    Callback callback_;
    std::atomic<bool> running_{false};

    std::string session_type_;
    unsigned int mod_mask_ = 0;
    unsigned int keycode_  = 0;

    struct Impl;
    Impl* impl_ = nullptr;
};
