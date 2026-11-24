// theme.c 在 Linux 上读取系统深浅色和强调色的部分。
// GTK 通过 settings portal 把系统的 color-scheme 反映到 gtk-application-prefer-dark-theme；
// 强调色直接问 xdg-desktop-portal 的 org.freedesktop.appearance.accent-color。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "theme.h"

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <math.h>

bool theme_linux_system_is_dark(void)
{
    GtkSettings *settings = gtk_settings_get_default();
    if (!settings) return false;
    gboolean prefer_dark = FALSE;
    g_object_get(settings, "gtk-application-prefer-dark-theme", &prefer_dark, NULL);
    return prefer_dark;
}

AccentPalette theme_linux_system_accent(void)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus) return accent_palette_default();

    GVariant *result = g_dbus_connection_call_sync(bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Settings", "Read", g_variant_new("(ss)", "org.freedesktop.appearance", "accent-color"), NULL,
        G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
    g_object_unref(bus);
    if (!result) return accent_palette_default();

    GVariant *value = NULL;
    g_variant_get(result, "(v)", &value);
    g_variant_unref(result);
    if (!value) return accent_palette_default();

    double red = 0, green = 0, blue = 0;
    gboolean ok = FALSE;
    if (g_variant_is_of_type(value, G_VARIANT_TYPE("(ddd)")))
    {
        g_variant_get(value, "(ddd)", &red, &green, &blue);
        ok = true;
    }
    g_variant_unref(value);
    if (!ok) return accent_palette_default();

    Color accent = COLOR_RGB((uint8_t)lround(red * 255), (uint8_t)lround(green * 255), (uint8_t)lround(blue * 255));
    return accent_palette_from_color(accent);
}