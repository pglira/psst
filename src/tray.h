#pragma once
#include <functional>

// Icon in the system tray. A click starts or stops dictation; the menu also
// opens the settings and quits psst.
class TrayIcon {
public:
    struct Actions {
        std::function<void()> toggle;
        std::function<void()> settings;
        std::function<void()> quit;
    };

    void init(Actions actions, const char* tooltip);

    // Show whether a recording runs.
    void set_recording(bool recording);

private:
    void update_icon();

    Actions actions_;
    int size_ = 0;           // icon size in pixels that the tray asks for
    bool recording_ = false;
    void* icon_ = nullptr;  // GtkStatusIcon
    void* menu_ = nullptr;  // GtkMenu
};
