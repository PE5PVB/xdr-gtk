#ifndef XDR_AUDIO_STREAM_H_
#define XDR_AUDIO_STREAM_H_
#include <glib.h>

/* Live-audio client for fm-dx-webserver.

   When xdr-gtk is connected to an fm-dx-webserver (conf.connection_mode == 2)
   the local audio bridge is meaningless (there is no physical tuner output on
   this machine). Instead we open a second WebSocket to the server's audio
   endpoint, receive encoded audio frames, decode them, and play them back
   through the WASAPI render device the user configured on the Audio Bridge
   settings page.

   Transport preference:
     1. /audio-opus  — WebSocket with custom framing
                       [u32 BE seq][u32 BE ts_hi][u32 BE ts_lo][opus payload]
                       Text messages: hello / config (OpusHead base64) / pong.
                       Client sends  ping JSON to keep NAT alive. Opus is
                       decoded with libopus.
     2. /audio       — 3LAS MP3 fallback. After the WS upgrade the client
                       sends a single text frame:
                           {"type":"fallback","data":"mp3"}
                       The server then pushes raw MP3 as binary frames.
                       Decoded with minimp3 (single-header, vendored).
   The fallback is triggered when the /audio-opus upgrade is rejected, the
   connection closes before the first binary frame, or libopus decoder init
   fails.

   All public functions are safe to call from the GTK main loop. Internally
   the stream starts dedicated connect + render threads.
*/

typedef enum
{
    AUDIO_STREAM_STOPPED = 0,
    AUDIO_STREAM_CONNECTING,
    AUDIO_STREAM_RUNNING,
    AUDIO_STREAM_ERROR
} audio_stream_status_t;

void audio_stream_init(void);
void audio_stream_shutdown(void);

/* Recomputes the desired stream state from conf / tuner.thread and starts or
   stops the worker threads as needed. Idempotent. Call after settings save,
   connect, and disconnect. */
void audio_stream_apply_state(void);

gboolean              audio_stream_is_running(void);
audio_stream_status_t audio_stream_get_status(void);
const gchar*          audio_stream_get_last_error(void);

/* Sets the render-session playback volume (0..100). Safe to call when the
   stream isn't running (no-op). */
void audio_stream_set_volume(gint volume_0_100);

#endif
