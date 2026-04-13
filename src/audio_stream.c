#include "audio_stream.h"
#include <string.h>

#ifdef G_OS_WIN32

#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <stdio.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <opus/opus.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "third_party/minimp3.h"

#include "conf.h"
#include "tuner.h"
#include "tuner-conn.h"

/* MinGW headers sometimes omit these flags; define fallbacks. */
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM       0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY  0x08000000
#endif

#define STREAM_LOG_PREFIX  "audio_stream:"
#define STREAM_BUFFER_MS    20    /* WASAPI endpoint buffer (lower = less latency, more underrun risk) */
#define STREAM_OPUS_WAIT_MS 3000  /* If no binary frames arrive within this many ms on /audio-opus, fall back. */

/* Queue watermarks in MILLISECONDS of audio. The target is ADAPTIVE: it
   starts at STREAM_TARGET_INIT_MS and is bumped up on every render-side
   underrun (jitter spike), then slowly decays back toward the initial
   value during underrun-free periods. This finds the smallest jitter
   buffer that the network + machine can sustain — low latency when the
   link is clean, larger buffer when it isn't. */
#define STREAM_TARGET_INIT_MS    40   /* lower bound / initial guess */
#define STREAM_TARGET_MAX_MS    400   /* hard upper bound */
#define STREAM_TARGET_BUMP_MS    30   /* grow on every underrun by this much */
#define STREAM_TARGET_DECAY_MS    5   /* shrink by this much per decay tick */
#define STREAM_DECAY_INTERVAL_US (10 * G_USEC_PER_SEC)  /* time between decay ticks */
#define STREAM_QUEUE_MAX_MULT     3   /* drop-oldest when depth exceeds target * this */

/* ----- GUIDs (duplicated from audio_bridge.c; kept local to keep this
   module self-contained). ----- */
DEFINE_GUID(STREAM_CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E,0x3D, 0xC4,0x57,0x92,0x91,0x69,0x2E);
DEFINE_GUID(STREAM_IID_IMMDeviceEnumerator,  0xA95664D2, 0x9614, 0x4F35, 0xA7,0x46, 0xDE,0x8D,0xB6,0x36,0x17,0xE6);
DEFINE_GUID(STREAM_IID_IAudioClient,         0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1,0x78, 0xC2,0xF5,0x68,0xA7,0x03,0xB2);
DEFINE_GUID(STREAM_IID_IAudioRenderClient,   0xF294ACFC, 0x3146, 0x4483, 0xA7,0xBF, 0xAD,0xDC,0xA7,0xC2,0x60,0xE2);
DEFINE_GUID(STREAM_IID_ISimpleAudioVolume,   0x87CE5498, 0x68D6, 0x44E5, 0x92,0x15, 0x6D,0xA4,0x7E,0xF8,0x83,0xD8);

typedef struct
{
    audio_stream_status_t status;
    gchar *last_error;
    gboolean running;

    /* Connect/decode thread reads from WS, pushes PCM packets here; render
       thread pops them and writes to WASAPI. */
    GThread     *worker_thread;
    GThread     *render_thread;
    gint         stop_request;   /* g_atomic_int_* */
    GCancellable *cancel;

    /* Output */
    IMMDevice           *render_device;
    IAudioClient        *render_client;
    IAudioRenderClient  *render_iface;
    ISimpleAudioVolume  *render_volume;
    HANDLE               render_event;
    WAVEFORMATEX        *format;    /* owned */
    guint                bytes_per_frame;
    guint                sample_rate;
    guint                channels;

    /* PCM queue: GByteArray* of interleaved s16 samples. queue_bytes tracks
       the sum of bytes currently in the queue so we can enforce watermarks
       in millisecond units without paying for walking the queue. */
    GAsyncQueue *pcm_queue;
    gint         queue_bytes;   /* g_atomic_int_* */

    /* Adaptive jitter-buffer target, in ms. Bumped by the render thread on
       underrun, decayed by the producer when the link is stable. */
    gint         target_ms;          /* g_atomic_int_* */
    gint64       last_decay_time;    /* g_get_monotonic_time() */
    gboolean     render_started;     /* ignore underruns before first real output */

    /* Set by the worker when it has proven — by receiving HTTP responses on
       both /audio-opus and /audio — that the server is reachable but the
       audio plugin isn't available. Auto-reconnect skips this case so we
       don't silently retry every few seconds. The flag clears on the next
       successful connect attempt (stream_start_internal zeroes it). */
    gint         audio_unavailable;  /* g_atomic_int_* */

    /* Chosen transport label for logging: "opus" or "mp3". */
    const gchar *transport;
} stream_state_t;

static stream_state_t stream;
static gboolean        com_initialized = FALSE;

/* Auto-restart bookkeeping — mirrors audio_bridge.c. When the worker thread
   exits before the user requested stop, we post a main-loop idle that
   tears down and reapplies state so the session reconnects. Rate-limited
   to avoid a reconnect loop on a permanently broken server. */
static gint    restart_pending;   /* g_atomic_int_* */
static gint64  last_restart_time; /* g_get_monotonic_time() */

/* Forward declarations */
static void   stream_stop_internal(void);
static void   set_error(const gchar *fmt, ...) G_GNUC_PRINTF(1, 2);
static void   clear_error(void);
static gboolean start_output(guint sample_rate, guint channels);
static void   queue_drain(GAsyncQueue *q);
static gpointer worker_thread_fn(gpointer data);
static gpointer render_thread_fn(gpointer data);
static void     note_underrun(void);
static gboolean stream_session_ended_idle(gpointer data);
static gboolean stream_start_internal(void);

/* ---------------------------------------------------------------- */
/* Status / error plumbing                                           */
/* ---------------------------------------------------------------- */

audio_stream_status_t
audio_stream_get_status(void)
{
    return stream.status;
}

const gchar*
audio_stream_get_last_error(void)
{
    return stream.last_error;
}

gboolean
audio_stream_is_running(void)
{
    return stream.running;
}

static void
clear_error(void)
{
    g_free(stream.last_error);
    stream.last_error = NULL;
}

static void
set_error(const gchar *fmt, ...)
{
    va_list ap;
    g_free(stream.last_error);
    va_start(ap, fmt);
    stream.last_error = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    stream.status = AUDIO_STREAM_ERROR;
    g_warning("%s %s", STREAM_LOG_PREFIX, stream.last_error);
}

void
audio_stream_init(void)
{
    if (!com_initialized)
    {
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        com_initialized = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }
    memset(&stream, 0, sizeof(stream));
    stream.status = AUDIO_STREAM_STOPPED;
}

void
audio_stream_shutdown(void)
{
    stream_stop_internal();
    clear_error();
}

/* ---------------------------------------------------------------- */
/* WASAPI output                                                     */
/* ---------------------------------------------------------------- */

static IMMDevice*
open_render_device_by_id(const gchar *utf8_id)
{
    if (!utf8_id || !*utf8_id) return NULL;

    IMMDeviceEnumerator *e = NULL;
    if (FAILED(CoCreateInstance(&STREAM_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &STREAM_IID_IMMDeviceEnumerator, (void**)&e)))
        return NULL;

    LPWSTR wide = (LPWSTR)g_utf8_to_utf16(utf8_id, -1, NULL, NULL, NULL);
    IMMDevice *dev = NULL;
    IMMDeviceEnumerator_GetDevice(e, wide, &dev);
    g_free(wide);
    IMMDeviceEnumerator_Release(e);
    return dev;
}

/* Initialize the WASAPI render client with a fixed s16/sr/ch format and let
   the Windows audio engine convert to the device mix format via
   AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM. Lets us ignore the device's native
   format entirely. */
static gboolean
start_output(guint sample_rate, guint channels)
{
    if (!conf.audio_bridge_output_id || !*conf.audio_bridge_output_id)
    {
        set_error("No output device selected on the Audio Bridge page");
        return FALSE;
    }

    stream.render_device = open_render_device_by_id(conf.audio_bridge_output_id);
    if (!stream.render_device)
    {
        set_error("Could not open output device (removed?)");
        return FALSE;
    }

    HRESULT hr = IMMDevice_Activate(stream.render_device, &STREAM_IID_IAudioClient,
                                    CLSCTX_ALL, NULL, (void**)&stream.render_client);
    if (FAILED(hr))
    {
        set_error("Activate(IAudioClient) failed (hr=0x%08lx)", (long)hr);
        return FALSE;
    }

    WAVEFORMATEX *fmt = CoTaskMemAlloc(sizeof(WAVEFORMATEX));
    fmt->wFormatTag      = WAVE_FORMAT_PCM;
    fmt->nChannels       = (WORD)channels;
    fmt->nSamplesPerSec  = sample_rate;
    fmt->wBitsPerSample  = 16;
    fmt->nBlockAlign     = (WORD)(channels * 2);
    fmt->nAvgBytesPerSec = sample_rate * channels * 2;
    fmt->cbSize          = 0;

    REFERENCE_TIME duration = (REFERENCE_TIME)STREAM_BUFFER_MS * 10000;
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    hr = IAudioClient_Initialize(stream.render_client, AUDCLNT_SHAREMODE_SHARED,
                                 flags, duration, 0, fmt, NULL);
    if (FAILED(hr))
    {
        CoTaskMemFree(fmt);
        set_error("render Initialize failed (hr=0x%08lx)", (long)hr);
        return FALSE;
    }
    stream.format = fmt;
    stream.bytes_per_frame = channels * 2;
    stream.sample_rate = sample_rate;
    stream.channels = channels;

    stream.render_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    IAudioClient_SetEventHandle(stream.render_client, stream.render_event);

    hr = IAudioClient_GetService(stream.render_client, &STREAM_IID_IAudioRenderClient,
                                 (void**)&stream.render_iface);
    if (FAILED(hr))
    {
        set_error("GetService(IAudioRenderClient) failed (hr=0x%08lx)", (long)hr);
        return FALSE;
    }

    hr = IAudioClient_GetService(stream.render_client, &STREAM_IID_ISimpleAudioVolume,
                                 (void**)&stream.render_volume);
    if (FAILED(hr)) stream.render_volume = NULL;

    if (stream.render_volume)
    {
        gint v = conf.volume;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        ISimpleAudioVolume_SetMasterVolume(stream.render_volume, (float)v / 100.0f, NULL);
        ISimpleAudioVolume_SetMute(stream.render_volume, FALSE, NULL);
    }

    IAudioClient_Start(stream.render_client);
    stream.render_thread = g_thread_new("xdr-stream-ren", render_thread_fn, NULL);
    return TRUE;
}

static void
queue_drain(GAsyncQueue *q)
{
    if (!q) return;
    GByteArray *p;
    while ((p = g_async_queue_try_pop(q)) != NULL)
        g_byte_array_unref(p);
}

static gpointer
render_thread_fn(gpointer data)
{
    (void)data;
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    UINT32 buffer_frames = 0;
    IAudioClient_GetBufferSize(stream.render_client, &buffer_frames);

    /* Currently being drained into WASAPI output. Render-thread-private. */
    GByteArray *current = NULL;
    gsize       current_off = 0;
    const guint bpf = stream.bytes_per_frame;

    while (!g_atomic_int_get(&stream.stop_request))
    {
        DWORD wait = WaitForSingleObject(stream.render_event, 200);
        if (wait != WAIT_OBJECT_0) continue;

        UINT32 padding = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(stream.render_client, &padding)))
            break;

        UINT32 frames_available = buffer_frames - padding;
        if (frames_available == 0) continue;

        BYTE *out = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(stream.render_iface, frames_available, &out)))
            break;

        const gsize wanted = (gsize)frames_available * bpf;
        gsize got = 0;
        while (got < wanted)
        {
            if (!current)
            {
                current = g_async_queue_try_pop(stream.pcm_queue);
                current_off = 0;
                if (!current) break;
                g_atomic_int_add(&stream.queue_bytes, -(gint)current->len);
                stream.render_started = TRUE;
            }
            gsize available = current->len - current_off;
            gsize to_copy = wanted - got;
            if (to_copy > available) to_copy = available;
            memcpy(out + got, current->data + current_off, to_copy);
            got += to_copy;
            current_off += to_copy;
            if (current_off >= current->len)
            {
                g_byte_array_unref(current);
                current = NULL;
                current_off = 0;
            }
        }
        if (got < wanted)
        {
            memset(out + got, 0, wanted - got);
            note_underrun();
        }

        IAudioRenderClient_ReleaseBuffer(stream.render_iface, frames_available, 0);
    }

    if (current) g_byte_array_unref(current);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
    return NULL;
}

static void
push_pcm(const gint16 *pcm, gsize frames)
{
    if (!stream.pcm_queue || frames == 0) return;
    const gsize bytes = frames * stream.bytes_per_frame;
    GByteArray *arr = g_byte_array_sized_new((guint)bytes);
    g_byte_array_append(arr, (const guint8*)pcm, (guint)bytes);
    g_async_queue_push(stream.pcm_queue, arr);
    g_atomic_int_add(&stream.queue_bytes, (gint)bytes);

    /* Enforce millisecond-based watermarks on the standing queue depth.
       Since the producer is ~realtime and the render thread consumes at
       exactly sample-rate, a sustained queue growth means clock drift in
       the direction of the server being faster than our output device —
       drop oldest packets to catch up. */
    const gint bytes_per_ms = (gint)(stream.sample_rate * stream.bytes_per_frame / 1000);
    if (bytes_per_ms <= 0) return;

    /* Periodic decay toward the initial (low-latency) target when things
       have been stable. Runs on the producer path so we don't need a timer. */
    gint64 now = g_get_monotonic_time();
    if (stream.last_decay_time == 0) stream.last_decay_time = now;
    if (now - stream.last_decay_time >= STREAM_DECAY_INTERVAL_US)
    {
        gint cur = g_atomic_int_get(&stream.target_ms);
        if (cur > STREAM_TARGET_INIT_MS)
        {
            gint next = cur - STREAM_TARGET_DECAY_MS;
            if (next < STREAM_TARGET_INIT_MS) next = STREAM_TARGET_INIT_MS;
            g_atomic_int_set(&stream.target_ms, next);
        }
        stream.last_decay_time = now;
    }

    const gint target_ms    = g_atomic_int_get(&stream.target_ms);
    const gint max_bytes    = target_ms * STREAM_QUEUE_MAX_MULT * bytes_per_ms;
    const gint target_bytes = target_ms * bytes_per_ms;
    if (g_atomic_int_get(&stream.queue_bytes) > max_bytes)
    {
        while (g_atomic_int_get(&stream.queue_bytes) > target_bytes)
        {
            GByteArray *old = g_async_queue_try_pop(stream.pcm_queue);
            if (!old) break;
            g_atomic_int_add(&stream.queue_bytes, -(gint)old->len);
            g_byte_array_unref(old);
        }
    }
}

/* Called by the render thread after a buffer fill came up short. Grows the
   adaptive target so future runs won't underrun at this jitter level. */
static void
note_underrun(void)
{
    if (!stream.render_started) return;   /* ignore prebuffer underruns */
    gint cur = g_atomic_int_get(&stream.target_ms);
    gint next = cur + STREAM_TARGET_BUMP_MS;
    if (next > STREAM_TARGET_MAX_MS) next = STREAM_TARGET_MAX_MS;
    if (next != cur)
    {
        g_atomic_int_set(&stream.target_ms, next);
        g_message("%s underrun — jitter buffer target %d → %d ms",
                  STREAM_LOG_PREFIX, cur, next);
    }
    /* Reset decay timer so we don't immediately start shrinking again. */
    stream.last_decay_time = g_get_monotonic_time();
}

/* ---------------------------------------------------------------- */
/* WebSocket connect                                                 */
/* ---------------------------------------------------------------- */

typedef struct
{
    GSocketConnection *sconn;
    GIOStream         *iostream;
    gboolean           upgraded;
} ws_conn_t;

static void
ws_conn_close(ws_conn_t *wc)
{
    if (!wc) return;
    if (wc->iostream) g_io_stream_close(wc->iostream, NULL, NULL);
    if (wc->sconn) g_object_unref(wc->sconn);
    memset(wc, 0, sizeof(*wc));
}

/* Connect + HTTP upgrade to ws[s]://host:port<path>.
   Returns the HTTP status code on success/failure of the upgrade:
     101  — upgrade succeeded (wc->upgraded == TRUE)
     4xx  — server responded but rejected the path / upgrade
     0    — could not reach the server at all (DNS/TCP/TLS failure). */
static gint
ws_connect(const gchar *host, const gchar *port, gboolean tls,
           const gchar *path, GCancellable *cancel, ws_conn_t *wc)
{
    memset(wc, 0, sizeof(*wc));

    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_tls(client, tls);
    g_socket_client_set_timeout(client, 15);
    GError *err = NULL;
    GSocketConnection *sconn = g_socket_client_connect_to_host(
        client, host, (guint16)atoi(port), cancel, &err);
    g_object_unref(client);
    if (!sconn)
    {
        if (err) g_error_free(err);
        return 0;
    }
    wc->sconn = sconn;
    wc->iostream = G_IO_STREAM(sconn);

    guchar nonce[16];
    for (int i = 0; i < 16; i++) nonce[i] = (guchar)g_random_int_range(0, 256);
    gchar *key_b64 = g_base64_encode(nonce, 16);
    gchar *req = g_strdup_printf(
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "User-Agent: xdr-gtk-stream\r\n"
        "\r\n",
        path, host, port, key_b64);
    g_free(key_b64);

    GOutputStream *out = g_io_stream_get_output_stream(wc->iostream);
    gboolean ok = stream_write_all(out, (const guchar*)req, strlen(req), cancel);
    g_free(req);
    if (!ok) return 0;

    gchar *resp = ws_read_http_response(wc->iostream, cancel);
    if (!resp) return 0;

    /* Parse "HTTP/1.1 <code> ..." */
    gint code = 0;
    const gchar *sp = strchr(resp, ' ');
    if (sp) code = atoi(sp + 1);
    g_free(resp);
    wc->upgraded = (code == 101);
    return code;
}

/* ---------------------------------------------------------------- */
/* Opus transport                                                    */
/* ---------------------------------------------------------------- */

/* Parse a single JSON top-level "type" string value. Tolerant parser:
   looks for "type":"..." and returns a newly-allocated string or NULL. */
static gchar*
json_extract_type(const gchar *json)
{
    const gchar *p = strstr(json, "\"type\"");
    if (!p) return NULL;
    p = strchr(p, ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    const gchar *q = strchr(p, '"');
    if (!q) return NULL;
    return g_strndup(p, q - p);
}

/* Returns TRUE if we started producing PCM on the render queue. FALSE means
   the caller should try the mp3 fallback. */
static gboolean
run_opus_session(ws_conn_t *wc, GCancellable *cancel)
{
    OpusDecoder *dec = NULL;
    int err = 0;
    guint channels = 2;   /* will be overridden by hello message */

    gint64 start_mono = g_get_monotonic_time();
    gboolean got_binary = FALSE;

    while (!g_atomic_int_get(&stream.stop_request))
    {
        /* Before first binary arrives, enforce an upper bound so we can fall
           back quickly if the server accepted the upgrade but never sends. */
        if (!got_binary &&
            (g_get_monotonic_time() - start_mono) > (gint64)STREAM_OPUS_WAIT_MS * 1000)
        {
            if (dec) opus_decoder_destroy(dec);
            return FALSE;
        }

        guchar *buf = NULL; gsize len = 0; guchar op = 0;
        gint r = ws_read_frame(wc->iostream, cancel, &buf, &len, &op);
        if (r < 0)
        {
            if (dec) opus_decoder_destroy(dec);
            return got_binary;
        }
        if (r == 0) continue;

        if (op == 0x1 && buf)  /* text */
        {
            gchar *text = g_strndup((const gchar*)buf, len);
            gchar *type = json_extract_type(text);
            if (type && strcmp(type, "hello") == 0)
            {
                /* Extract channels from hello. Accept both "channels":1 and "channels":2. */
                const gchar *c = strstr(text, "\"channels\"");
                if (c)
                {
                    c = strchr(c, ':');
                    if (c) { c++; while (*c==' '||*c=='\t') c++; channels = (guint)atoi(c); }
                }
                if (channels != 1 && channels != 2) channels = 2;
            }
            g_free(type);
            g_free(text);
            g_free(buf);
            continue;
        }

        if (op != 0x2)  /* not binary, skip */
        {
            g_free(buf);
            continue;
        }

        /* First binary frame: init decoder + output now that we know channels. */
        if (!dec)
        {
            dec = opus_decoder_create(48000, (int)channels, &err);
            if (err != OPUS_OK || !dec)
            {
                g_free(buf);
                return FALSE;
            }
            if (!start_output(48000, channels))
            {
                opus_decoder_destroy(dec);
                g_free(buf);
                return TRUE;  /* treat as terminal error; don't fall back. */
            }
            got_binary = TRUE;
            stream.running = TRUE;
            stream.status = AUDIO_STREAM_RUNNING;
            stream.transport = "opus";
            g_message("%s connected via /audio-opus (%u ch, 48 kHz)",
                      STREAM_LOG_PREFIX, channels);
        }

        /* 12-byte header: [u32 BE seq][u32 BE ts_hi][u32 BE ts_lo][opus]. */
        if (len < 12) { g_free(buf); continue; }
        const guchar *payload = buf + 12;
        gsize plen = len - 12;

        /* 10 ms @ 48 kHz = 480 samples/ch. Allow up to 120 ms for safety. */
        int max_samples = 120 * 48;
        gint16 *pcm = g_malloc((gsize)max_samples * channels * sizeof(gint16));
        int frames = opus_decode(dec, payload, (opus_int32)plen, pcm, max_samples, 0);
        if (frames > 0) push_pcm(pcm, (gsize)frames);
        g_free(pcm);
        g_free(buf);
    }

    if (dec) opus_decoder_destroy(dec);
    return got_binary;
}

/* ---------------------------------------------------------------- */
/* MP3 (3LAS) transport                                              */
/* ---------------------------------------------------------------- */

static gboolean
run_mp3_session(ws_conn_t *wc, GCancellable *cancel)
{
    /* Send fallback request */
    const gchar *req = "{\"type\":\"fallback\",\"data\":\"mp3\"}";
    if (!ws_send_masked_frame(wc->iostream, cancel, 0x1,
                              (const guchar*)req, strlen(req)))
        return FALSE;

    mp3dec_t mp3;
    mp3dec_init(&mp3);

    /* Growing accumulator of MP3 bytes; decoder needs a contiguous window. */
    GByteArray *acc = g_byte_array_sized_new(16384);
    gint16 *pcm = g_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(gint16));
    gboolean started = FALSE;

    while (!g_atomic_int_get(&stream.stop_request))
    {
        guchar *buf = NULL; gsize len = 0; guchar op = 0;
        gint r = ws_read_frame(wc->iostream, cancel, &buf, &len, &op);
        if (r < 0) break;
        if (r == 0) continue;

        if (op != 0x2) { g_free(buf); continue; }

        g_byte_array_append(acc, buf, (guint)len);
        g_free(buf);

        /* Repeatedly decode frames that are now complete in the accumulator. */
        gsize consumed_total = 0;
        while (consumed_total < acc->len)
        {
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame(&mp3,
                                              acc->data + consumed_total,
                                              (int)(acc->len - consumed_total),
                                              pcm, &info);
            if (info.frame_bytes == 0)
                break;  /* Need more data */
            consumed_total += info.frame_bytes;
            if (samples == 0) continue;  /* Skipped ID3 or similar */

            if (!started)
            {
                if (!start_output((guint)info.hz, (guint)info.channels))
                {
                    g_byte_array_free(acc, TRUE);
                    g_free(pcm);
                    return TRUE;  /* terminal */
                }
                started = TRUE;
                stream.running = TRUE;
                stream.status = AUDIO_STREAM_RUNNING;
                stream.transport = "mp3";
                g_message("%s connected via /audio (mp3 %d Hz, %d ch, %d kbps)",
                          STREAM_LOG_PREFIX,
                          info.hz, info.channels, info.bitrate_kbps);
            }
            push_pcm(pcm, (gsize)samples);
        }
        if (consumed_total > 0)
            g_byte_array_remove_range(acc, 0, (guint)consumed_total);
    }

    g_byte_array_free(acc, TRUE);
    g_free(pcm);
    return started;
}

/* ---------------------------------------------------------------- */
/* Worker (connect) thread                                           */
/* ---------------------------------------------------------------- */

static gpointer
worker_thread_fn(gpointer data)
{
    (void)data;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    gchar *host = NULL, *port = NULL, *path_unused = NULL;
    gboolean tls = FALSE;
    if (!ws_parse_url(conf.webserver_url ? conf.webserver_url : "",
                      &host, &port, &path_unused, &tls))
    {
        set_error("Invalid fm-dx-webserver URL");
        goto done;
    }
    g_free(path_unused);

    gint opus_code = 0;
    gint mp3_code  = 0;

    /* Try opus first. */
    {
        ws_conn_t wc;
        opus_code = ws_connect(host, port, tls, "/audio-opus", stream.cancel, &wc);
        if (wc.upgraded)
        {
            gboolean produced = run_opus_session(&wc, stream.cancel);
            ws_conn_close(&wc);
            if (produced || g_atomic_int_get(&stream.stop_request))
                goto done;
            g_message("%s /audio-opus yielded no audio, falling back to mp3",
                      STREAM_LOG_PREFIX);
        }
        else
        {
            ws_conn_close(&wc);
        }
    }

    if (g_atomic_int_get(&stream.stop_request)) goto done;

    /* Fallback: mp3 */
    {
        ws_conn_t wc;
        mp3_code = ws_connect(host, port, tls, "/audio", stream.cancel, &wc);
        if (!wc.upgraded)
        {
            ws_conn_close(&wc);
            /* Both transports rejected with an HTTP status means the server
               answered (so it IS a reachable fm-dx-webserver) but the audio
               plugin / endpoint is not wired up. Distinguish from network
               errors so the UI + auto-reconnect logic can react properly. */
            if (opus_code > 0 && mp3_code > 0)
            {
                g_atomic_int_set(&stream.audio_unavailable, 1);
                set_error("fm-dx-webserver reached but audio streaming is "
                          "not available (plugin disabled or server doesn't "
                          "publish /audio-opus or /audio — HTTP %d / %d)",
                          opus_code, mp3_code);
            }
            else
            {
                set_error("Could not reach fm-dx-webserver audio endpoint "
                          "(opus=%d, mp3=%d)", opus_code, mp3_code);
            }
            goto done;
        }
        run_mp3_session(&wc, stream.cancel);
        ws_conn_close(&wc);
    }

done:
    g_free(host);
    g_free(port);
    CoUninitialize();

    /* Worker is about to exit. If the user didn't request a stop, kick the
       main loop to tear the session down and try to reconnect. We can't
       touch stream.* further here because the idle handler will. */
    if (!g_atomic_int_get(&stream.stop_request))
    {
        if (g_atomic_int_compare_and_exchange(&restart_pending, 0, 1))
            g_idle_add(stream_session_ended_idle, NULL);
    }
    return NULL;
}

/* ---------------------------------------------------------------- */
/* Lifecycle                                                         */
/* ---------------------------------------------------------------- */

static void
stream_stop_internal(void)
{
    g_atomic_int_set(&stream.stop_request, 1);
    if (stream.cancel) g_cancellable_cancel(stream.cancel);

    if (stream.render_client) IAudioClient_Stop(stream.render_client);

    if (stream.worker_thread) { g_thread_join(stream.worker_thread); stream.worker_thread = NULL; }
    if (stream.render_thread) { g_thread_join(stream.render_thread); stream.render_thread = NULL; }

    if (stream.render_iface) { IAudioRenderClient_Release(stream.render_iface); stream.render_iface = NULL; }
    if (stream.render_volume){ ISimpleAudioVolume_Release(stream.render_volume); stream.render_volume = NULL; }
    if (stream.render_client){ IAudioClient_Release(stream.render_client); stream.render_client = NULL; }
    if (stream.render_device){ IMMDevice_Release(stream.render_device); stream.render_device = NULL; }
    if (stream.render_event) { CloseHandle(stream.render_event); stream.render_event = NULL; }
    if (stream.format)       { CoTaskMemFree(stream.format); stream.format = NULL; }

    if (stream.pcm_queue)
    {
        queue_drain(stream.pcm_queue);
        g_async_queue_unref(stream.pcm_queue);
        stream.pcm_queue = NULL;
    }
    g_atomic_int_set(&stream.queue_bytes, 0);

    if (stream.cancel) { g_object_unref(stream.cancel); stream.cancel = NULL; }

    stream.running = FALSE;
    stream.transport = NULL;
    g_atomic_int_set(&stream.stop_request, 0);
    stream.bytes_per_frame = 0;
    if (stream.status != AUDIO_STREAM_ERROR)
        stream.status = AUDIO_STREAM_STOPPED;
}

static gboolean
stream_start_internal(void)
{
    if (stream.running) return TRUE;
    clear_error();

    if (!conf.webserver_url || !*conf.webserver_url)
    {
        set_error("No fm-dx-webserver URL configured");
        return FALSE;
    }
    if (!conf.audio_bridge_output_id || !*conf.audio_bridge_output_id)
    {
        set_error("No output device selected on the Audio Bridge page");
        return FALSE;
    }

    stream.pcm_queue = g_async_queue_new();
    stream.cancel = g_cancellable_new();
    g_atomic_int_set(&stream.stop_request, 0);
    g_atomic_int_set(&stream.queue_bytes, 0);
    g_atomic_int_set(&stream.target_ms, STREAM_TARGET_INIT_MS);
    g_atomic_int_set(&stream.audio_unavailable, 0);
    stream.render_started = FALSE;
    stream.last_decay_time = 0;
    stream.status = AUDIO_STREAM_CONNECTING;

    stream.worker_thread = g_thread_new("xdr-stream-ws", worker_thread_fn, NULL);
    return TRUE;
}

static gboolean
stream_session_ended_idle(gpointer data)
{
    (void)data;
    g_atomic_int_set(&restart_pending, 0);

    /* Worker exited on its own (server closed WS, decoder failed, etc.).
       Tear the session down fully so apply_state sees a clean STOPPED
       state, then (if the user still wants audio and we haven't been
       reconnecting in a tight loop) start a fresh session. */
    stream_stop_internal();

    gint64 now = g_get_monotonic_time();
    gboolean rate_ok = (now - last_restart_time) > 2 * G_USEC_PER_SEC;

    gboolean want = (conf.connection_mode == 2)
                    && tuner.thread
                    && conf.webserver_url
                    && *conf.webserver_url
                    && conf.audio_bridge_output_id
                    && *conf.audio_bridge_output_id;

    /* If we already confirmed the server doesn't publish an audio endpoint,
       there's no point reconnecting — the user would have to change the
       server's configuration first. Surface the error and stop. */
    gboolean plugin_missing = g_atomic_int_get(&stream.audio_unavailable) != 0;

    if (want && rate_ok && !plugin_missing)
    {
        last_restart_time = now;
        g_message("%s auto-reconnecting after unexpected session end",
                  STREAM_LOG_PREFIX);
        stream_start_internal();
    }
    else if (plugin_missing)
    {
        g_message("%s audio plugin unavailable on server — not reconnecting",
                  STREAM_LOG_PREFIX);
    }
    return G_SOURCE_REMOVE;
}

void
audio_stream_apply_state(void)
{
    gboolean want = (conf.connection_mode == 2)
                    && tuner.thread
                    && conf.webserver_url
                    && *conf.webserver_url
                    && conf.audio_bridge_output_id
                    && *conf.audio_bridge_output_id;

    if (want && !stream.worker_thread)
    {
        clear_error();
        stream.status = AUDIO_STREAM_STOPPED;
        stream_start_internal();
    }
    else if (!want && (stream.worker_thread || stream.running))
    {
        stream_stop_internal();
        clear_error();
    }
}

void
audio_stream_set_volume(gint volume_0_100)
{
    if (!stream.running || !stream.render_volume) return;
    if (volume_0_100 < 0) volume_0_100 = 0;
    if (volume_0_100 > 100) volume_0_100 = 100;
    ISimpleAudioVolume_SetMasterVolume(stream.render_volume,
                                       (float)volume_0_100 / 100.0f, NULL);
}

#else /* !G_OS_WIN32 */

void audio_stream_init(void) {}
void audio_stream_shutdown(void) {}
void audio_stream_apply_state(void) {}
gboolean audio_stream_is_running(void) { return FALSE; }
audio_stream_status_t audio_stream_get_status(void) { return AUDIO_STREAM_STOPPED; }
const gchar* audio_stream_get_last_error(void) { return NULL; }
void audio_stream_set_volume(gint v) { (void)v; }

#endif
