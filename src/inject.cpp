#include "inject.h"
#include "hotkey.h"
#include <iostream>
#include <cstdlib>
#include <cstdio>

#ifdef HAS_XDO
extern "C" {
#include <xdo.h>
}
#endif

static void inject_x11(const std::string& text) {
#ifdef HAS_XDO
    xdo_t* xdo = xdo_new(nullptr);
    if (!xdo) {
        std::cerr << "[inject] Failed to init libxdo\n";
        return;
    }

    // Copy text to clipboard via xclip
    FILE* proc = popen("xclip -selection clipboard", "w");
    if (proc) {
        fwrite(text.data(), 1, text.size(), proc);
        pclose(proc);
    } else {
        std::cerr << "[inject] Failed to run xclip\n";
        xdo_free(xdo);
        return;
    }

    // Small delay to let modifier keys release fully
    usleep(200'000);

    // Paste via Ctrl+V
    xdo_send_keysequence_window(xdo, CURRENTWINDOW, "ctrl+v", 12'000);
    xdo_free(xdo);
    std::cerr << "[inject] Pasted via xclip + xdotool (" << text.size() << " chars)\n";
#else
    // Fallback: pipe through xclip + xdotool
    FILE* proc = popen("xclip -selection clipboard", "w");
    if (proc) {
        fwrite(text.data(), 1, text.size(), proc);
        pclose(proc);
    } else {
        std::cerr << "[inject] Failed to run xclip\n";
        return;
    }

    usleep(200'000);
    int ret = std::system("xdotool key --clearmodifiers ctrl+v");
    if (ret != 0)
        std::cerr << "[inject] xdotool key failed (code " << ret << ")\n";
    else
        std::cerr << "[inject] Pasted via xclip + xdotool CLI (" << text.size() << " chars)\n";
#endif
}

static void inject_wayland(const std::string& text) {
    // Copy text to clipboard via wl-copy
    FILE* proc = popen("wl-copy", "w");
    if (!proc) {
        std::cerr << "[inject] Failed to run wl-copy\n";
        return;
    }
    fwrite(text.data(), 1, text.size(), proc);
    pclose(proc);

    // Paste via wtype Ctrl+V
    usleep(100'000);
    int ret = std::system("wtype -k ctrl+v");
    if (ret != 0)
        std::cerr << "[inject] wtype paste failed (code " << ret << ")\n";
    else
        std::cerr << "[inject] Pasted via wl-copy + wtype (" << text.size() << " chars)\n";
}

void inject_text(const std::string& text) {
    if (text.empty()) return;

    std::string session = HotkeyListener::detect_session();
    if (session == "wayland")
        inject_wayland(text);
    else
        inject_x11(text);
}

void inject_clipboard(const std::string& text) {
    if (text.empty()) return;

    std::string session = HotkeyListener::detect_session();
    const char* cmd = (session == "wayland") ? "wl-copy" : "xclip -selection clipboard";

    FILE* proc = popen(cmd, "w");
    if (!proc) {
        std::cerr << "[inject] Failed to run " << cmd << "\n";
        return;
    }
    fwrite(text.data(), 1, text.size(), proc);
    pclose(proc);
    std::cerr << "[inject] Copied to clipboard (" << text.size() << " chars)\n";
}
