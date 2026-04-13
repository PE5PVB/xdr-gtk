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

/* ----- Debug snapshot (used by the debug window) -----
   All counters are cumulative since the last successful start; they reset
   on each bridge start. Timestamps are in milliseconds (g_get_monotonic_time
   divided by 1000) to make them easy to subtract. Safe to call from any
   thread; values are read via g_atomic_int_get. */
typedef struct audio_bridge_debug_snapshot
{
    audio_bridge_status_t status;
    const gchar *last_error;   /* weak ref owned by the bridge */

    gint cap_events;           /* capture_event wakeups that delivered data */
    gint cap_frames;            /* frames pushed into the ring */
    gint cap_frames_silent;     /* frames flagged AUDCLNT_BUFFERFLAGS_SILENT */
    gint cap_last_ms;           /* ms timestamp of the last successful capture event */

    gint ren_events;            /* render_event wakeups that produced output */
    gint ren_frames;            /* frames consumed from the ring */
    gint ren_underruns;         /* frames of silence padding on underrun */
    gint ren_last_ms;           /* ms timestamp of the last successful render event */

    gint drift_stretch;         /* count of "low_fill" frame duplications */
    gint drift_skip;            /* count of "high_fill" frame drops */

    gint restarts;              /* count of auto-restart attempts */

    gint start_ms;              /* ms timestamp when the current run started */
    gint now_ms;                /* ms timestamp at snapshot time */

    gint ring_capacity;         /* bytes */
    gint ring_fill;              /* bytes currently in the ring */

    guint format_sample_rate;   /* 0 if not started */
    guint format_channels;
    guint format_bits;

    /* 0..100 integer peak amplitude of the last packet seen by each thread.
       Zero while no data has been processed yet, or genuinely silent audio. */
    gint cap_peak;
    gint ren_peak;

    /* Max peak seen in the current poll window (reset by the snapshot).
       These reveal whether the audio source is *continuously* quiet or
       just has occasional silent packets mixed with loud ones. */
    gint cap_peak_win_max;
    gint ren_peak_win_max;

    /* Peak computed by peeking at the ring's raw bytes at the read
       position, just BEFORE ring_read consumes them. If this is > 0
       while ren_peak is 0, the bug is in ring_read / drift-comp. If
       both are 0, the ring genuinely contains zeros. */
    gint ring_peek_peak_win_max;

    /* Count of near-silent packets (peak <= 2) seen since the last
       snapshot reset. Paired with total packet count gives us a "what
       fraction of the stream was quiet" metric. */
    gint cap_quiet_packets;
    gint cap_total_packets;
    gint ren_quiet_packets;
    gint ren_total_packets;

    /* Live read-back of the render stream's per-session volume / mute.
       render_volume_pct is -1 if the query failed or the bridge isn't running. */
    gint     render_volume_pct;
    gboolean render_muted;
} audio_bridge_debug_snapshot_t;

void audio_bridge_debug_get(audio_bridge_debug_snapshot_t *out);

#endif
