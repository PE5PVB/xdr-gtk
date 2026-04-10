#ifndef XDR_AUDIO_BRIDGE_H_
#define XDR_AUDIO_BRIDGE_H_
#include <glib.h>

/* Lightweight WASAPI shared-mode audio router used to bridge a capture
   device (e.g. the sound card connected to the tuner's audio output) to a
   render device (e.g. the user's speakers/headphones). Routing only runs
   while the radio connection is active and the user has enabled the bridge.

   Format conversion is intentionally NOT supported in this minimal version:
   if the chosen capture and render devices have incompatible mix formats,
   the bridge will fail to start with an error logged to stderr. The user
   must set matching formats in Windows Sound settings for both devices.

   All public functions are safe to call from the GTK main loop. Internally
   the bridge starts dedicated capture and render threads on Start. */

typedef struct audio_bridge_device
{
    gchar *id;    /* Persistent WASAPI endpoint id (UTF-8) */
    gchar *name;  /* Friendly name for display (UTF-8) */
} audio_bridge_device_t;

typedef enum
{
    AUDIO_BRIDGE_STOPPED = 0,
    AUDIO_BRIDGE_RUNNING,
    AUDIO_BRIDGE_ERROR
} audio_bridge_status_t;

/* Returns the current bridge status (cheap, safe from main thread). */
audio_bridge_status_t audio_bridge_get_status(void);

/* Last error message produced by a failed start, or NULL. The returned
   string is owned by the bridge and stays valid until the next state
   change; copy it if you need to keep it. */
const gchar* audio_bridge_get_last_error(void);

/* Sets the playback volume on the bridge's render stream (0..100).
   Only the per-stream volume is touched; the system mixer is not affected.
   Safe to call when the bridge is not running (no-op). */
void audio_bridge_set_volume(gint volume_0_100);

/* Initializes COM (single-threaded apartment per-thread is handled inside
   the bridge threads). Safe to call multiple times. */
void audio_bridge_init(void);

/* Stops any running bridge and releases resources. Call at shutdown. */
void audio_bridge_shutdown(void);

/* Enumerates audio capture (input == TRUE) or render (input == FALSE)
   endpoints. Returns a newly-allocated GList of audio_bridge_device_t*
   that the caller must free with audio_bridge_device_list_free(). */
GList* audio_bridge_enumerate_devices(gboolean input);
void   audio_bridge_device_list_free(GList *list);

/* Recomputes the desired bridge state from the current configuration and
   the radio connection state, and starts/stops the routing threads as
   needed. Idempotent. Call this after:
     - settings save
     - tuner connect (when tuner.thread becomes valid)
     - tuner disconnect
*/
void audio_bridge_apply_state(void);

/* Returns TRUE while the bridge threads are actively routing audio. */
gboolean audio_bridge_is_running(void);

#endif
