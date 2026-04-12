#include "tray.h"
#include <iostream>

#ifdef HAS_AYATANA
#include <libayatana-appindicator/app-indicator.h>
// app-indicator.h pulls in GTK3's gtk/gtk.h
#endif

struct TrayIcon::Impl {
#ifdef HAS_AYATANA
    AppIndicator* indicator = nullptr;
    GtkWidget* menu = nullptr;
#endif
};

bool TrayIcon::init(const Config& cfg, const std::string& icon_dir) {
    if (!cfg.tray_enabled) {
        std::cerr << "[tray] Disabled in config\n";
        return false;
    }

#ifdef HAS_AYATANA
    impl_ = new Impl;

    impl_->indicator = app_indicator_new(
        "psst",
        "audio-input-microphone",
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS
    );

    app_indicator_set_status(impl_->indicator, APP_INDICATOR_STATUS_ACTIVE);

    // Try to set custom icon path
    if (!icon_dir.empty())
        app_indicator_set_icon_theme_path(impl_->indicator, icon_dir.c_str());

    // Build menu
    impl_->menu = gtk_menu_new();

    GtkWidget* item_status = gtk_menu_item_new_with_label("Status: Idle");
    gtk_widget_set_sensitive(item_status, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(impl_->menu), item_status);

    GtkWidget* sep = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(impl_->menu), sep);

    GtkWidget* item_quit = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(item_quit, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer data) {
        auto* self = static_cast<TrayIcon*>(data);
        if (self->quit_cb_) self->quit_cb_();
    }), this);
    gtk_menu_shell_append(GTK_MENU_SHELL(impl_->menu), item_quit);

    gtk_widget_show_all(impl_->menu);
    app_indicator_set_menu(impl_->indicator, GTK_MENU(impl_->menu));

    std::cerr << "[tray] Initialized\n";
    return true;
#else
    (void)icon_dir;
    std::cerr << "[tray] Built without ayatana-appindicator support\n";
    return false;
#endif
}

void TrayIcon::set_state(State state) {
#ifdef HAS_AYATANA
    if (!impl_ || !impl_->indicator) return;

    const char* icon = "audio-input-microphone";
    switch (state) {
        case State::Idle:
            icon = "audio-input-microphone";
            break;
        case State::Recording:
            icon = "media-record";
            break;
        case State::Transcribing:
            icon = "system-run";
            break;
    }
    app_indicator_set_icon(impl_->indicator, icon);
#else
    (void)state;
#endif
}

void TrayIcon::shutdown() {
#ifdef HAS_AYATANA
    if (impl_) {
        if (impl_->indicator)
            g_object_unref(impl_->indicator);
        delete impl_;
        impl_ = nullptr;
    }
#endif
}
