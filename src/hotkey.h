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

    // Listen for `bind` instead of the current binding. Returns false if
    // `bind` is invalid or another program holds the key; the current
    // binding then stays.
    bool rebind(const std::string& bind);

    // Returns "x11" or "wayland" based on detected session.
    static std::string detect_session();

    ~HotkeyListener();

private:
    void listen_x11();
    // Parse `bind` into mod_mask_ and keycode_; keycode_ is 0 if invalid.
    void parse_binding(const std::string& bind);

    Config cfg_;
    Callback callback_;
    std::atomic<bool> running_{false};
    std::atomic<int> grab_result_{-1};  // -1 pending, 0 failed, 1 grabbed

    std::string session_type_;
    unsigned int mod_mask_ = 0;
    unsigned int keycode_  = 0;

    struct Impl;
    Impl* impl_ = nullptr;
};
