#include "inject.h"
#include "hotkey.h"
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>

#ifdef HAS_XDO
extern "C" {
#include <xdo.h>
}
#endif

static void inject_x11(size_t backspaces, const std::string& text, int type_delay_ms) {
#ifdef HAS_XDO
    xdo_t* xdo = xdo_new(nullptr);
    if (!xdo) {
        std::cerr << "[inject] Failed to init libxdo\n";
        return;
    }

    // Let modifier keys from the hotkey release before typing.
    usleep(200'000);

    // Clear any still-held modifiers (e.g. Super from the hotkey) so typed
    // characters don't get combined into shortcuts. Restore after typing.
    charcodemap_t* active_mods = nullptr;
    int n_active_mods = 0;
    xdo_get_active_modifiers(xdo, &active_mods, &n_active_mods);
    xdo_clear_active_modifiers(xdo, CURRENTWINDOW, active_mods, n_active_mods);

    useconds_t delay_us = static_cast<useconds_t>(type_delay_ms) * 1000;
    for (size_t i = 0; i < backspaces; ++i)
        xdo_send_keysequence_window(xdo, CURRENTWINDOW, "BackSpace", delay_us);
    if (!text.empty())
        xdo_enter_text_window(xdo, CURRENTWINDOW, text.c_str(), delay_us);

    xdo_set_active_modifiers(xdo, CURRENTWINDOW, active_mods, n_active_mods);
    if (active_mods) free(active_mods);
    xdo_free(xdo);
    std::cerr << "[inject] Typed via libxdo (" << backspaces << " backspaces, "
              << text.size() << " chars)\n";
#else
    usleep(200'000);
    if (backspaces > 0) {
        std::string keys = "xdotool key --clearmodifiers --delay " +
                           std::to_string(type_delay_ms) + " --repeat " +
                           std::to_string(backspaces) + " BackSpace";
        if (std::system(keys.c_str()) != 0)
            std::cerr << "[inject] xdotool key failed\n";
    }
    if (text.empty()) return;
    char cmd[64];
    std::snprintf(cmd, sizeof(cmd),
                  "xdotool type --clearmodifiers --delay %d --file -",
                  type_delay_ms);
    FILE* proc = popen(cmd, "w");
    if (!proc) {
        std::cerr << "[inject] Failed to run xdotool\n";
        return;
    }
    fwrite(text.data(), 1, text.size(), proc);
    int ret = pclose(proc);
    if (ret != 0)
        std::cerr << "[inject] xdotool type failed (code " << ret << ")\n";
    else
        std::cerr << "[inject] Typed via xdotool CLI (" << text.size() << " chars)\n";
#endif
}

static void inject_wayland(size_t backspaces, const std::string& text) {
    usleep(100'000);
    if (backspaces > 0) {
        std::string keys = "wtype";
        for (size_t i = 0; i < backspaces; ++i) keys += " -k BackSpace";
        if (std::system(keys.c_str()) != 0)
            std::cerr << "[inject] wtype key failed\n";
    }
    if (text.empty()) return;
    FILE* proc = popen("wtype -", "w");
    if (!proc) {
        std::cerr << "[inject] Failed to run wtype\n";
        return;
    }
    fwrite(text.data(), 1, text.size(), proc);
    int ret = pclose(proc);
    if (ret != 0)
        std::cerr << "[inject] wtype failed (code " << ret << ")\n";
    else
        std::cerr << "[inject] Typed via wtype (" << text.size() << " chars)\n";
}

void inject_text(const std::string& text, int type_delay_ms) {
    inject_edit(0, text, type_delay_ms);
}

void inject_edit(size_t backspaces, const std::string& text, int type_delay_ms) {
    if (backspaces == 0 && text.empty()) return;

    std::string session = HotkeyListener::detect_session();
    if (session == "wayland")
        inject_wayland(backspaces, text);
    else
        inject_x11(backspaces, text, type_delay_ms);
}

void inject_replace(const std::string& before, const std::string& after,
                    int type_delay_ms) {
    size_t common = 0;
    while (common < before.size() && common < after.size() &&
           before[common] == after[common])
        ++common;
    // Step back to the start of a UTF-8 character.
    while (common > 0 && common < before.size() &&
           (static_cast<unsigned char>(before[common]) & 0xC0) == 0x80)
        --common;

    size_t backspaces = 0;
    for (size_t i = common; i < before.size(); ++i)
        if ((static_cast<unsigned char>(before[i]) & 0xC0) != 0x80) ++backspaces;

    inject_edit(backspaces, after.substr(common), type_delay_ms);
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
