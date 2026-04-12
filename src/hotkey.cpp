#include "hotkey.h"
#include <iostream>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifdef HAS_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#endif

struct HotkeyListener::Impl {
    std::thread thread;
#ifdef HAS_X11
    Display* display = nullptr;
#endif
};

std::string HotkeyListener::detect_session() {
    const char* type = std::getenv("XDG_SESSION_TYPE");
    if (type) {
        std::string s(type);
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        if (s == "wayland") {
            // Check if XWayland is available
            if (std::getenv("DISPLAY"))
                return "x11"; // XWayland available — use X11 path
            return "wayland";
        }
        return s;
    }
    if (std::getenv("WAYLAND_DISPLAY")) return "wayland";
    if (std::getenv("DISPLAY")) return "x11";
    return "unknown";
}

void HotkeyListener::parse_binding(const std::string& bind) {
#ifdef HAS_X11
    mod_mask_ = 0;
    keycode_  = 0;

    // Split by '+'
    std::string lower = bind;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    size_t pos = 0;
    std::string part;
    std::string key_name;
    while (pos < lower.size()) {
        auto next = lower.find('+', pos);
        if (next == std::string::npos) {
            key_name = lower.substr(pos);
            break;
        }
        part = lower.substr(pos, next - pos);
        pos = next + 1;

        if (part == "super" || part == "mod" || part == "mod4")
            mod_mask_ |= Mod4Mask;
        else if (part == "ctrl" || part == "control")
            mod_mask_ |= ControlMask;
        else if (part == "alt" || part == "mod1")
            mod_mask_ |= Mod1Mask;
        else if (part == "shift")
            mod_mask_ |= ShiftMask;
    }

    if (key_name.empty()) {
        std::cerr << "[hotkey] No key specified in binding: " << bind << "\n";
        return;
    }

    // Convert key name to keysym
    // Handle single character keys
    KeySym sym = NoSymbol;
    if (key_name.size() == 1) {
        sym = XStringToKeysym(key_name.c_str());
        if (sym == NoSymbol) {
            // Try uppercase
            std::string upper = key_name;
            upper[0] = (char)toupper(upper[0]);
            sym = XStringToKeysym(upper.c_str());
        }
    } else {
        sym = XStringToKeysym(key_name.c_str());
    }

    if (sym == NoSymbol) {
        std::cerr << "[hotkey] Unknown key: " << key_name << "\n";
        return;
    }

    keycode_ = XKeysymToKeycode(impl_->display, sym);
    std::cerr << "[hotkey] Parsed binding: " << bind
              << " → mod=0x" << std::hex << mod_mask_
              << " keycode=" << std::dec << keycode_ << "\n";
#endif
}

bool HotkeyListener::init(const Config& cfg, Callback on_toggle) {
    cfg_ = cfg;
    callback_ = std::move(on_toggle);
    impl_ = new Impl;

    session_type_ = detect_session();
    std::cerr << "[hotkey] Session type: " << session_type_ << "\n";

    if (session_type_ == "x11") {
#ifdef HAS_X11
        impl_->display = XOpenDisplay(nullptr);
        if (!impl_->display) {
            std::cerr << "[hotkey] Failed to open X11 display\n";
            return false;
        }
        parse_binding(cfg.hotkey_bind);
        if (keycode_ == 0) return false;
        return true;
#else
        std::cerr << "[hotkey] Built without X11 support\n";
        return false;
#endif
    } else if (session_type_ == "wayland") {
        std::cerr << "[hotkey] Pure Wayland detected (no XWayland).\n"
                  << "  Global hotkeys are not supported natively.\n"
                  << "  Workaround: bind your compositor key to run:\n"
                  << "    whisper-hotkey --toggle\n";
        return false;
    }

    std::cerr << "[hotkey] Unknown session type: " << session_type_ << "\n";
    return false;
}

void HotkeyListener::start() {
    if (session_type_ == "x11") {
        running_.store(true);
        impl_->thread = std::thread(&HotkeyListener::listen_x11, this);
    }
}

void HotkeyListener::listen_x11() {
#ifdef HAS_X11
    Window root = DefaultRootWindow(impl_->display);

    // Grab with various lock-key combinations (NumLock, CapsLock, ScrollLock)
    unsigned int lock_masks[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    for (auto lm : lock_masks) {
        XGrabKey(impl_->display, (int)keycode_, mod_mask_ | lm, root,
                 False, GrabModeAsync, GrabModeAsync);
    }
    XSelectInput(impl_->display, root, KeyPressMask);
    XFlush(impl_->display);

    std::cerr << "[hotkey] Listening for " << cfg_.hotkey_bind << "\n";

    while (running_.load()) {
        // Use a short timeout so we can check running_ periodically
        while (XPending(impl_->display) > 0 && running_.load()) {
            XEvent ev;
            XNextEvent(impl_->display, &ev);
            if (ev.type == KeyPress) {
                if (callback_) callback_();
            }
        }
        // Sleep briefly to avoid busy-spinning
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    for (auto lm : lock_masks) {
        XUngrabKey(impl_->display, (int)keycode_, mod_mask_ | lm, root);
    }
    XFlush(impl_->display);
#endif
}

void HotkeyListener::stop() {
    running_.store(false);
    if (impl_ && impl_->thread.joinable())
        impl_->thread.join();
}

HotkeyListener::~HotkeyListener() {
    stop();
    if (impl_) {
#ifdef HAS_X11
        if (impl_->display)
            XCloseDisplay(impl_->display);
#endif
        delete impl_;
    }
}
