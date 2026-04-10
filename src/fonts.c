#include <glib.h>
#include <gio/gio.h>
#include "fonts.h"

#ifdef G_OS_WIN32
#include <windows.h>
#endif

static const char *embedded_font_paths[] = {
    "/org/xdr-gtk/fonts/JetBrainsMono-ExtraLight.ttf",
    "/org/xdr-gtk/fonts/JetBrainsMono-Light.ttf",
    "/org/xdr-gtk/fonts/JetBrainsMono-Regular.ttf",
    "/org/xdr-gtk/fonts/JetBrainsMono-Medium.ttf",
    "/org/xdr-gtk/fonts/Inter-Regular.ttf",
    "/org/xdr-gtk/fonts/Inter-Medium.ttf",
    "/org/xdr-gtk/fonts/Inter-Bold.ttf",
};

/* Keep loaded GBytes alive for the lifetime of the process so the font data
   pointers handed to the OS remain valid. */
static GBytes *loaded_font_bytes[G_N_ELEMENTS(embedded_font_paths)];

void
fonts_load_embedded(void)
{
    gsize i;
    for (i = 0; i < G_N_ELEMENTS(embedded_font_paths); i++)
    {
        GError *error = NULL;
        GBytes *bytes = g_resources_lookup_data(embedded_font_paths[i], 0, &error);
        if (!bytes)
        {
            g_warning("fonts: failed to load %s: %s",
                      embedded_font_paths[i],
                      error ? error->message : "unknown error");
            g_clear_error(&error);
            continue;
        }

        loaded_font_bytes[i] = bytes;

#ifdef G_OS_WIN32
        {
            gsize size = 0;
            gconstpointer data = g_bytes_get_data(bytes, &size);
            DWORD installed = 0;
            HANDLE handle = AddFontMemResourceEx((PVOID)data, (DWORD)size, NULL, &installed);
            if (!handle || installed == 0)
                g_warning("fonts: AddFontMemResourceEx failed for %s", embedded_font_paths[i]);
        }
#endif
    }
}
