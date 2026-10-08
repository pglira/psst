#include "tray.h"
#include <gtk/gtk.h>

// GtkStatusIcon is deprecated in GTK 3 but is the only tray API that GTK 3
// has; it works with XEmbed system trays (XFCE, MATE, KDE, ...).
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

namespace {

void rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0,         G_PI / 2);
    cairo_arc(cr, x + r,     y + h - r, r, G_PI / 2,  G_PI);
    cairo_arc(cr, x + r,     y + r,     r, G_PI,      3 * G_PI / 2);
    cairo_close_path(cr);
}

// A white microphone on a rounded square: blue when idle, red while recording.
GdkPixbuf* draw_icon(int size, bool recording) {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t* cr = cairo_create(surface);
    double s = size;

    rounded_rect(cr, s * 0.04, s * 0.04, s * 0.92, s * 0.92, s * 0.24);
    if (recording) cairo_set_source_rgb(cr, 0.90, 0.22, 0.24);  // #e5383d
    else           cairo_set_source_rgb(cr, 0.0, 0.45, 0.64);   // #0072a3
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    // Capsule
    rounded_rect(cr, s * 0.38, s * 0.18, s * 0.24, s * 0.40, s * 0.12);
    cairo_fill(cr);
    // Holder arc, stem and base
    cairo_set_line_width(cr, s * 0.07);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_arc(cr, s * 0.5, s * 0.44, s * 0.21, 0, G_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, s * 0.5, s * 0.66);
    cairo_line_to(cr, s * 0.5, s * 0.78);
    cairo_move_to(cr, s * 0.37, s * 0.80);
    cairo_line_to(cr, s * 0.63, s * 0.80);
    cairo_stroke(cr);

    cairo_destroy(cr);
    GdkPixbuf* pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, size, size);
    cairo_surface_destroy(surface);
    return pixbuf;
}

} // namespace

void TrayIcon::init(Actions actions, const char* tooltip) {
    actions_ = std::move(actions);

    GtkStatusIcon* icon = gtk_status_icon_new();
    gtk_status_icon_set_title(icon, "psst");
    gtk_status_icon_set_tooltip_text(icon, tooltip);
    icon_ = icon;
    update_icon();
    g_signal_connect(icon, "size-changed",
                     G_CALLBACK(+[](GtkStatusIcon*, gint size, gpointer data) -> gboolean {
                         auto* self = static_cast<TrayIcon*>(data);
                         self->size_ = size;
                         self->update_icon();
                         return TRUE;
                     }), this);

    GtkWidget* menu = gtk_menu_new();
    auto add_item = [&](const char* label, std::function<void()>* action) {
        GtkWidget* item = gtk_menu_item_new_with_label(label);
        g_signal_connect(item, "activate",
                         G_CALLBACK(+[](GtkMenuItem*, gpointer data) {
                             auto* fn = static_cast<std::function<void()>*>(data);
                             if (*fn) (*fn)();
                         }), action);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    };
    add_item("Start / stop dictation", &actions_.toggle);
    add_item("Settings…", &actions_.settings);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    add_item("Quit", &actions_.quit);
    gtk_widget_show_all(menu);
    menu_ = menu;

    g_signal_connect(icon, "activate",
                     G_CALLBACK(+[](GtkStatusIcon*, gpointer data) {
                         auto* self = static_cast<TrayIcon*>(data);
                         if (self->actions_.toggle) self->actions_.toggle();
                     }), this);
    g_signal_connect(icon, "popup-menu",
                     G_CALLBACK(+[](GtkStatusIcon*, guint, guint, gpointer data) {
                         auto* self = static_cast<TrayIcon*>(data);
                         gtk_menu_popup_at_pointer(GTK_MENU(self->menu_), nullptr);
                     }), this);
}

void TrayIcon::set_recording(bool recording) {
    recording_ = recording;
    update_icon();
}

void TrayIcon::update_icon() {
    if (!icon_) return;
    GdkPixbuf* pixbuf = draw_icon(size_ > 0 ? size_ : 22, recording_);
    gtk_status_icon_set_from_pixbuf(GTK_STATUS_ICON(icon_), pixbuf);
    g_object_unref(pixbuf);
}

G_GNUC_END_IGNORE_DEPRECATIONS
