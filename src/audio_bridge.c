#include "audio_bridge.h"
#include <string.h>

#ifdef G_OS_WIN32

#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <stdio.h>
#include <string.h>

#include "conf.h"
#include "tuner.h"

#define BRIDGE_BUFFER_MS    50    /* WASAPI buffer duration request */
#define BRIDGE_QUEUE_MAX    60    /* drop oldest once more than this many packets queued (~600 ms @ 10 ms/packet) */
#define BRIDGE_QUEUE_TARGET 30    /* when dropping, shrink down to this depth (~300 ms) */
#define BRIDGE_LOG_PREFIX   "audio_bridge:"

/* ----- Audio queue -----
   Replaces the old lock-free ring buffer. Capture packets are allocated as
   GByteArray and pushed onto a GAsyncQueue; the render thread pops them,
   copies the samples into the WASAPI render buffer, and unrefs. If the
   queue depth exceeds BRIDGE_QUEUE_MAX (producer is faster than consumer
   due to clock drift or WASAPI backpressure) the oldest packets are
   dropped down to BRIDGE_QUEUE_TARGET — that's our "drift compensation":
   crude but guaranteed glitch-free for small drifts, and produces a
   single small gap for large drifts instead of corrupting the stream. */

/* ----- Bridge state ----- */

typedef struct
{
    gboolean         running;
    audio_bridge_status_t status;
    gchar           *last_error;

    GThread         *capture_thread;
    GThread         *render_thread;
    gint             stop_request; /* g_atomic_int_* */

    IMMDevice       *capture_device;
    IMMDevice       *render_device;
    IAudioClient    *capture_client;
    IAudioClient    *render_client;
    IAudioCaptureClient *capture_iface;
    IAudioRenderClient  *render_iface;
    ISimpleAudioVolume  *capture_volume;
    ISimpleAudioVolume  *render_volume;
    HANDLE           capture_event;
    HANDLE           render_event;

    WAVEFORMATEX    *format;
    UINT32           bytes_per_frame;

    /* Audio queue: GByteArray* packets, FIFO. Thread-safe via glib. */
    GAsyncQueue     *audio_queue;
    /* Render-thread private state: the packet currently being drained into
       the WASAPI render buffer and how many bytes of it have been consumed
       so far. Never touched from other threads. */
    GByteArray      *render_current;
    gsize            render_current_offset;

    /* Wide-string copies of the active device IDs, used to compare against
       hot-plug notifications which arrive as LPCWSTR. */
    LPWSTR           capture_id_w;
    LPWSTR           render_id_w;

    /* ----- Debug counters (all updated via g_atomic_int_*) -----
       These feed the debug window. Reset in bridge_start_internal, bumped
       in the capture/render threads. The _ms timestamps are monotonic time
       / 1000. */
    gint dbg_cap_events;
    gint dbg_cap_frames;
    gint dbg_cap_frames_silent;
    gint dbg_cap_last_ms;
    gint dbg_ren_events;
    gint dbg_ren_frames;
    gint dbg_ren_underruns;
    gint dbg_ren_last_ms;
    gint dbg_drift_stretch;
    gint dbg_drift_skip;
    gint dbg_restarts;
    gint dbg_start_ms;
    gint dbg_cap_peak;   /* 0..100 from the last non-silent capture packet */
    gint dbg_ren_peak;   /* 0..100 from the last render output buffer */

    /* Per-window tracking (reset by the snapshot getter on each poll). */
    gint dbg_cap_peak_win_max;
    gint dbg_ren_peak_win_max;
    gint dbg_ring_peek_peak_win_max;
    gint dbg_cap_quiet_packets;
    gint dbg_cap_total_packets;
    gint dbg_ren_quiet_packets;
    gint dbg_ren_total_packets;
} bridge_state_t;

/* Atomic max update: write new_value to *target iff it's greater than the
   current value. Diagnostic helper; tiny race on reset is acceptable. */
static inline void
atomic_max(gint *target, gint new_value)
{
    gint cur;
    do {
        cur = g_atomic_int_get(target);
        if (new_value <= cur) return;
    } while (!g_atomic_int_compare_and_exchange(target, cur, new_value));
}

static bridge_state_t bridge;
static gboolean       com_initialized = FALSE;

/* Auto-recovery: when capture or render threads detect a WASAPI failure or
   a prolonged event stall they post a main-loop idle to tear down and
   re-start the bridge. restart_pending prevents duplicate posts, and the
   timestamp provides a simple 2-second rate limit so a permanently broken
   device can't spin forever. */
static gint    restart_pending;      /* g_atomic_int_* */
static gint64  last_restart_time;    /* g_get_monotonic_time() of last attempt */

/* IMMNotificationClient implementation (manual COM-in-C vtable) so we can
   detect device removal / disable while the bridge is running. */
typedef struct bridge_notify
{
    IMMNotificationClientVtbl *lpVtbl;
    LONG ref;
} bridge_notify_t;

static bridge_notify_t       *notify_client = NULL;
static IMMDeviceEnumerator   *notify_enumerator = NULL;

/* Forward declarations to keep the file in topical order. */
static IMMDeviceEnumerator* get_enumerator(void);
static void                 bridge_stop_internal(void);
static void                 hotplug_register(void);
static void                 hotplug_unregister(void);

/* ----- COM helpers ----- */

DEFINE_GUID(BRIDGE_CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E,0x3D, 0xC4,0x57,0x92,0x91,0x69,0x2E);
DEFINE_GUID(BRIDGE_IID_IMMDeviceEnumerator,  0xA95664D2, 0x9614, 0x4F35, 0xA7,0x46, 0xDE,0x8D,0xB6,0x36,0x17,0xE6);
DEFINE_GUID(BRIDGE_IID_IAudioClient,         0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1,0x78, 0xC2,0xF5,0x68,0xA7,0x03,0xB2);
DEFINE_GUID(BRIDGE_IID_IAudioCaptureClient,  0xC8ADBD64, 0xE71E, 0x48A0, 0xA4,0xDE, 0x18,0x5C,0x39,0x5C,0xD3,0x17);
DEFINE_GUID(BRIDGE_IID_IAudioRenderClient,   0xF294ACFC, 0x3146, 0x4483, 0xA7,0xBF, 0xAD,0xDC,0xA7,0xC2,0x60,0xE2);
DEFINE_GUID(BRIDGE_IID_ISimpleAudioVolume,   0x87CE5498, 0x68D6, 0x44E5, 0x92,0x15, 0x6D,0xA4,0x7E,0xF8,0x83,0xD8);

static gchar*
wide_to_utf8(LPCWSTR w)
{
    if (!w) return NULL;
    return g_utf16_to_utf8((const gunichar2*)w, -1, NULL, NULL, NULL);
}

static LPWSTR
utf8_to_wide(const gchar *s)
{
    if (!s) return NULL;
    return (LPWSTR)g_utf8_to_utf16(s, -1, NULL, NULL, NULL);
}

static void
log_hr(const char *what, HRESULT hr)
{
    g_warning("%s %s failed (hr=0x%08lx)", BRIDGE_LOG_PREFIX, what, (long)hr);
}

static void
set_error(const gchar *fmt, ...)
{
    va_list ap;
    g_free(bridge.last_error);
    va_start(ap, fmt);
    bridge.last_error = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    bridge.status = AUDIO_BRIDGE_ERROR;
    g_warning("%s %s", BRIDGE_LOG_PREFIX, bridge.last_error);
}

static void
clear_error(void)
{
    g_free(bridge.last_error);
    bridge.last_error = NULL;
}

audio_bridge_status_t
audio_bridge_get_status(void)
{
    return bridge.status;
}

const gchar*
audio_bridge_get_last_error(void)
{
    return bridge.last_error;
}

/* ----- IMMNotificationClient (hot-plug detection) ----- */

static gboolean
hotplug_apply_state_idle(gpointer data)
{
    audio_bridge_apply_state();
    return G_SOURCE_REMOVE;
}

static void
hotplug_check_and_stop(LPCWSTR pwstrDeviceId)
{
    if (!bridge.running || !pwstrDeviceId) return;
    if ((bridge.capture_id_w && wcscmp(bridge.capture_id_w, pwstrDeviceId) == 0) ||
        (bridge.render_id_w  && wcscmp(bridge.render_id_w,  pwstrDeviceId) == 0))
    {
        set_error("Audio device removed or disabled, bridge stopped");
        /* Marshal the actual stop to the main loop, since this callback
           runs on a COM worker thread. */
        g_idle_add(hotplug_apply_state_idle, NULL);
        /* Force apply_state to see "want=FALSE" by clearing running flag
           lazily — we just rely on the next apply_state from main thread
           which will check device state. Set status now so the UI shows it. */
    }
}

static HRESULT STDMETHODCALLTYPE
notify_QueryInterface(IMMNotificationClient *self, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IMMNotificationClient))
    {
        *out = self;
        ((bridge_notify_t*)self)->lpVtbl->AddRef(self);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
notify_AddRef(IMMNotificationClient *self)
{
    return InterlockedIncrement(&((bridge_notify_t*)self)->ref);
}

static ULONG STDMETHODCALLTYPE
notify_Release(IMMNotificationClient *self)
{
    return InterlockedDecrement(&((bridge_notify_t*)self)->ref);
}

static HRESULT STDMETHODCALLTYPE
notify_OnDeviceStateChanged(IMMNotificationClient *self, LPCWSTR pwstrDeviceId, DWORD dwNewState)
{
    (void)self;
    if (dwNewState != DEVICE_STATE_ACTIVE)
        hotplug_check_and_stop(pwstrDeviceId);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
notify_OnDeviceAdded(IMMNotificationClient *self, LPCWSTR pwstrDeviceId)
{
    (void)self; (void)pwstrDeviceId;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
notify_OnDeviceRemoved(IMMNotificationClient *self, LPCWSTR pwstrDeviceId)
{
    (void)self;
    hotplug_check_and_stop(pwstrDeviceId);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
notify_OnDefaultDeviceChanged(IMMNotificationClient *self, EDataFlow flow, ERole role, LPCWSTR pwstrDefault)
{
    (void)self; (void)flow; (void)role; (void)pwstrDefault;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
notify_OnPropertyValueChanged(IMMNotificationClient *self, LPCWSTR pwstrDeviceId, const PROPERTYKEY key)
{
    (void)self; (void)pwstrDeviceId; (void)key;
    return S_OK;
}

static IMMNotificationClientVtbl notify_vtbl = {
    notify_QueryInterface,
    notify_AddRef,
    notify_Release,
    notify_OnDeviceStateChanged,
    notify_OnDeviceAdded,
    notify_OnDeviceRemoved,
    notify_OnDefaultDeviceChanged,
    notify_OnPropertyValueChanged,
};

static void
hotplug_register(void)
{
    if (notify_client) return;
    notify_enumerator = get_enumerator();
    if (!notify_enumerator) return;
    notify_client = g_new0(bridge_notify_t, 1);
    notify_client->lpVtbl = &notify_vtbl;
    notify_client->ref = 1;
    HRESULT hr = IMMDeviceEnumerator_RegisterEndpointNotificationCallback(
        notify_enumerator, (IMMNotificationClient*)notify_client);
    if (FAILED(hr))
    {
        log_hr("RegisterEndpointNotificationCallback", hr);
        g_free(notify_client);
        notify_client = NULL;
        IMMDeviceEnumerator_Release(notify_enumerator);
        notify_enumerator = NULL;
    }
}

static void
hotplug_unregister(void)
{
    if (!notify_client) return;
    if (notify_enumerator)
    {
        IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(
            notify_enumerator, (IMMNotificationClient*)notify_client);
        IMMDeviceEnumerator_Release(notify_enumerator);
        notify_enumerator = NULL;
    }
    g_free(notify_client);
    notify_client = NULL;
}

/* ----- Public init / shutdown ----- */

void
audio_bridge_init(void)
{
    if (!com_initialized)
    {
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        com_initialized = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }
    memset(&bridge, 0, sizeof(bridge));
    bridge.status = AUDIO_BRIDGE_STOPPED;
    hotplug_register();
}

void
audio_bridge_shutdown(void)
{
    bridge_stop_internal();
    hotplug_unregister();
    clear_error();
    if (com_initialized)
    {
        CoUninitialize();
        com_initialized = FALSE;
    }
}

/* ----- Device enumeration ----- */

static IMMDeviceEnumerator*
get_enumerator(void)
{
    IMMDeviceEnumerator *e = NULL;
    HRESULT hr = CoCreateInstance(&BRIDGE_CLSID_MMDeviceEnumerator,
                                  NULL,
                                  CLSCTX_ALL,
                                  &BRIDGE_IID_IMMDeviceEnumerator,
                                  (void**)&e);
    if (FAILED(hr))
    {
        log_hr("CoCreateInstance(MMDeviceEnumerator)", hr);
        return NULL;
    }
    return e;
}

GList*
audio_bridge_enumerate_devices(gboolean input)
{
    GList *list = NULL;
    IMMDeviceEnumerator *e = get_enumerator();
    if (!e) return NULL;

    IMMDeviceCollection *coll = NULL;
    EDataFlow flow = input ? eCapture : eRender;
    HRESULT hr = IMMDeviceEnumerator_EnumAudioEndpoints(e, flow, DEVICE_STATE_ACTIVE, &coll);
    if (FAILED(hr))
    {
        log_hr("EnumAudioEndpoints", hr);
        IMMDeviceEnumerator_Release(e);
        return NULL;
    }

    UINT count = 0;
    IMMDeviceCollection_GetCount(coll, &count);
    for (UINT i = 0; i < count; i++)
    {
        IMMDevice *dev = NULL;
        if (FAILED(IMMDeviceCollection_Item(coll, i, &dev)) || !dev)
            continue;

        LPWSTR id_w = NULL;
        if (FAILED(IMMDevice_GetId(dev, &id_w)))
        {
            IMMDevice_Release(dev);
            continue;
        }

        IPropertyStore *props = NULL;
        gchar *name = NULL;
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &props)) && props)
        {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(IPropertyStore_GetValue(props, &PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
                name = wide_to_utf8(v.pwszVal);
            PropVariantClear(&v);
            IPropertyStore_Release(props);
        }

        audio_bridge_device_t *d = g_new0(audio_bridge_device_t, 1);
        d->id = wide_to_utf8(id_w);
        d->name = name ? name : g_strdup("(unknown)");
        list = g_list_append(list, d);

        CoTaskMemFree(id_w);
        IMMDevice_Release(dev);
    }

    IMMDeviceCollection_Release(coll);
    IMMDeviceEnumerator_Release(e);
    return list;
}

void
audio_bridge_device_list_free(GList *list)
{
    for (GList *l = list; l; l = l->next)
    {
        audio_bridge_device_t *d = l->data;
        g_free(d->id);
        g_free(d->name);
        g_free(d);
    }
    g_list_free(list);
}

/* ----- Device opening + format negotiation ----- */

static IMMDevice*
open_device_by_id(const gchar *utf8_id)
{
    if (!utf8_id || !*utf8_id) return NULL;
    IMMDeviceEnumerator *e = get_enumerator();
    if (!e) return NULL;

    LPWSTR wide = utf8_to_wide(utf8_id);
    IMMDevice *dev = NULL;
    HRESULT hr = IMMDeviceEnumerator_GetDevice(e, wide, &dev);
    g_free(wide);
    IMMDeviceEnumerator_Release(e);
    if (FAILED(hr))
    {
        log_hr("GetDevice", hr);
        return NULL;
    }
    return dev;
}

static gboolean
formats_match(const WAVEFORMATEX *a, const WAVEFORMATEX *b)
{
    if (!a || !b) return FALSE;
    if (a->wFormatTag != b->wFormatTag) return FALSE;
    if (a->nChannels != b->nChannels) return FALSE;
    if (a->nSamplesPerSec != b->nSamplesPerSec) return FALSE;
    if (a->wBitsPerSample != b->wBitsPerSample) return FALSE;
    return TRUE;
}

/* ----- Peak amplitude (diagnostic) -----
   Walks a raw audio buffer and returns the peak sample value in the range
   0..100 (integer percent of full scale). Supports 32-bit IEEE float and
   16/32-bit PCM. Unknown formats return 0 rather than a bogus value. */

static gint
compute_peak_percent(const void *data, gsize bytes, const WAVEFORMATEX *fmt)
{
    if (!data || bytes == 0 || !fmt) return 0;

    /* Extensible 32-bit is nearly always IEEE float on modern Windows, so
       accept that without cracking open the SubFormat GUID. */
    const gboolean is_float =
        (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
        (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->wBitsPerSample == 32);

    if (is_float && fmt->wBitsPerSample == 32)
    {
        const float *s = (const float*)data;
        const gsize n = bytes / sizeof(float);
        float p = 0.0f;
        for (gsize i = 0; i < n; i++)
        {
            float v = s[i];
            if (v < 0.0f) v = -v;
            if (v > p) p = v;
        }
        if (p > 1.0f) p = 1.0f;
        return (gint)(p * 100.0f + 0.5f);
    }

    if (fmt->wBitsPerSample == 16)
    {
        const gint16 *s = (const gint16*)data;
        const gsize n = bytes / sizeof(gint16);
        gint p = 0;
        for (gsize i = 0; i < n; i++)
        {
            gint v = s[i];
            if (v < 0) v = -v;
            if (v > p) p = v;
        }
        return (gint)((gint64)p * 100 / 32768);
    }

    if (fmt->wBitsPerSample == 32)
    {
        const gint32 *s = (const gint32*)data;
        const gsize n = bytes / sizeof(gint32);
        gint32 p = 0;
        for (gsize i = 0; i < n; i++)
        {
            gint32 v = s[i];
            if (v < 0) v = -v;
            if (v > p) p = v;
        }
        return (gint)((gint64)p * 100 / 2147483648LL);
    }

    return 0;
}

/* ----- Diagnostic queue peek -----
   Called from the render thread only. Returns the peak of the packet
   that render is about to play (the one currently in bridge.render_current).
   When render has no current packet, returns 0. */

static gint
queue_peek_peak(const WAVEFORMATEX *fmt)
{
    if (!bridge.render_current) return 0;
    if (bridge.render_current_offset >= bridge.render_current->len) return 0;
    gsize len = bridge.render_current->len - bridge.render_current_offset;
    const guint8 *data = bridge.render_current->data + bridge.render_current_offset;
    if (len > 2048) len = 2048;
    return compute_peak_percent(data, len, fmt);
}

/* Drain and free every queued packet. Safe to call on a NULL queue. */
static void
queue_drain(GAsyncQueue *q)
{
    if (!q) return;
    GByteArray *pkt;
    while ((pkt = g_async_queue_try_pop(q)) != NULL)
        g_byte_array_unref(pkt);
}

/* ----- Auto-restart on WASAPI failure / stall ----- */

static gboolean
bridge_restart_idle(gpointer data)
{
    (void)data;
    g_atomic_int_set(&restart_pending, 0);

    if (!bridge.running)
        return G_SOURCE_REMOVE;

    g_message("%s restarting bridge (auto-recovery)", BRIDGE_LOG_PREFIX);
    bridge_stop_internal();
    audio_bridge_apply_state();
    return G_SOURCE_REMOVE;
}

static void
request_restart(const gchar *reason)
{
    gint64 now = g_get_monotonic_time();
    /* Rate-limit to at most one restart attempt per 2 seconds. */
    if (now - last_restart_time < 2000000)
        return;
    last_restart_time = now;

    if (g_atomic_int_compare_and_exchange(&restart_pending, 0, 1))
    {
        g_atomic_int_inc(&bridge.dbg_restarts);
        g_warning("%s auto-restart: %s", BRIDGE_LOG_PREFIX, reason);
        g_idle_add(bridge_restart_idle, NULL);
    }
}

/* ----- Capture / render threads ----- */

static gpointer
capture_thread_fn(gpointer data)
{
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    HRESULT hr_com = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr_com;

    gint timeout_count = 0;

    while (!g_atomic_int_get(&bridge.stop_request))
    {
        DWORD wait = WaitForSingleObject(bridge.capture_event, 200);
        if (wait != WAIT_OBJECT_0)
        {
            /* 10 timeouts × 200ms = 2s without a capture event → stalled. */
            if (++timeout_count > 10)
            {
                request_restart("capture stalled (no events)");
                goto capture_exit;
            }
            continue;
        }
        timeout_count = 0;

        g_atomic_int_inc(&bridge.dbg_cap_events);
        g_atomic_int_set(&bridge.dbg_cap_last_ms,
                         (gint)(g_get_monotonic_time() / 1000));

        UINT32 packet = 0;
        HRESULT hr;
        while (SUCCEEDED(hr = IAudioCaptureClient_GetNextPacketSize(bridge.capture_iface, &packet)) && packet > 0)
        {
            BYTE  *audio = NULL;
            UINT32 frames = 0;
            DWORD  flags = 0;
            HRESULT ghr = IAudioCaptureClient_GetBuffer(bridge.capture_iface, &audio, &frames, &flags, NULL, NULL);
            if (FAILED(ghr))
            {
                log_hr("capture GetBuffer", ghr);
                request_restart("capture GetBuffer failed");
                goto capture_exit;
            }

            gsize bytes = (gsize)frames * bridge.bytes_per_frame;
            gint packet_peak;
            GByteArray *arr = g_byte_array_sized_new((guint)bytes);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
            {
                g_atomic_int_add(&bridge.dbg_cap_frames_silent, (gint)frames);
                packet_peak = 0;
                /* Append zeros of the right size so render pops a packet
                   of the expected length. */
                g_byte_array_set_size(arr, (guint)bytes);
                memset(arr->data, 0, bytes);
            }
            else
            {
                g_byte_array_append(arr, (const guint8*)audio, (guint)bytes);
                packet_peak = compute_peak_percent(audio, bytes, bridge.format);
            }
            g_async_queue_push(bridge.audio_queue, arr);

            /* Drop oldest if queue grew past the upper bound (drift or
               render stall). Shrink down to target depth in one go. */
            if (g_async_queue_length(bridge.audio_queue) > BRIDGE_QUEUE_MAX)
            {
                while (g_async_queue_length(bridge.audio_queue) > BRIDGE_QUEUE_TARGET)
                {
                    GByteArray *old = g_async_queue_try_pop(bridge.audio_queue);
                    if (!old) break;
                    g_atomic_int_inc(&bridge.dbg_drift_skip);
                    g_byte_array_unref(old);
                }
            }

            g_atomic_int_set(&bridge.dbg_cap_peak, packet_peak);
            atomic_max(&bridge.dbg_cap_peak_win_max, packet_peak);
            g_atomic_int_inc(&bridge.dbg_cap_total_packets);
            if (packet_peak <= 2)
                g_atomic_int_inc(&bridge.dbg_cap_quiet_packets);
            g_atomic_int_add(&bridge.dbg_cap_frames, (gint)frames);

            IAudioCaptureClient_ReleaseBuffer(bridge.capture_iface, frames);
        }

        if (FAILED(hr))
        {
            log_hr("capture GetNextPacketSize", hr);
            request_restart("capture GetNextPacketSize failed");
            goto capture_exit;
        }
    }

capture_exit:
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
    return NULL;
}

static gpointer
render_thread_fn(gpointer data)
{
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr;

    UINT32 buffer_frames = 0;
    IAudioClient_GetBufferSize(bridge.render_client, &buffer_frames);

    /* Drift is handled on the capture side (dropping oldest queued packets
       when the queue grows past BRIDGE_QUEUE_MAX). The render side just
       pops packets as WASAPI asks for output and pads with silence on
       underrun. */
    const gint bpf = bridge.bytes_per_frame;
    gint timeout_count = 0;

    while (!g_atomic_int_get(&bridge.stop_request))
    {
        DWORD wait = WaitForSingleObject(bridge.render_event, 200);
        if (wait != WAIT_OBJECT_0)
        {
            /* 10 timeouts × 200ms = 2s without a render event → stalled. */
            if (++timeout_count > 10)
            {
                request_restart("render stalled (no events)");
                goto render_exit;
            }
            continue;
        }
        timeout_count = 0;

        UINT32 padding = 0;
        HRESULT hr = IAudioClient_GetCurrentPadding(bridge.render_client, &padding);
        if (FAILED(hr))
        {
            log_hr("render GetCurrentPadding", hr);
            request_restart("render GetCurrentPadding failed");
            goto render_exit;
        }

        UINT32 frames_available = buffer_frames - padding;
        if (frames_available == 0) continue;

        BYTE *out = NULL;
        hr = IAudioRenderClient_GetBuffer(bridge.render_iface, frames_available, &out);
        if (FAILED(hr))
        {
            log_hr("render GetBuffer", hr);
            request_restart("render GetBuffer failed");
            goto render_exit;
        }

        g_atomic_int_inc(&bridge.dbg_ren_events);
        g_atomic_int_set(&bridge.dbg_ren_last_ms,
                         (gint)(g_get_monotonic_time() / 1000));

        const gsize wanted = (gsize)frames_available * bpf;
        gsize got;

        /* Diagnostic: peek at the current packet before we consume it
           further. If bridge.render_current is exhausted this returns 0. */
        {
            gint peek = queue_peek_peak(bridge.format);
            atomic_max(&bridge.dbg_ring_peek_peak_win_max, peek);
        }

        /* Pop packets from the queue and copy into the WASAPI output buffer
           until we've filled `wanted` bytes or the queue runs dry. */
        got = 0;
        while (got < wanted)
        {
            if (!bridge.render_current)
            {
                bridge.render_current = g_async_queue_try_pop(bridge.audio_queue);
                bridge.render_current_offset = 0;
                if (!bridge.render_current) break;
            }
            gsize available = bridge.render_current->len - bridge.render_current_offset;
            gsize to_copy = wanted - got;
            if (to_copy > available) to_copy = available;
            memcpy((guint8*)out + got,
                   bridge.render_current->data + bridge.render_current_offset,
                   to_copy);
            got += to_copy;
            bridge.render_current_offset += to_copy;
            if (bridge.render_current_offset >= bridge.render_current->len)
            {
                g_byte_array_unref(bridge.render_current);
                bridge.render_current = NULL;
                bridge.render_current_offset = 0;
            }
        }

        g_atomic_int_add(&bridge.dbg_ren_frames, (gint)frames_available);

        if (got < wanted)
        {
            memset(out + got, 0, wanted - got); /* hard underrun */
            g_atomic_int_add(&bridge.dbg_ren_underruns,
                             (gint)((wanted - got) / (gsize)bpf));
        }

        {
            gint pk = compute_peak_percent(out, wanted, bridge.format);
            g_atomic_int_set(&bridge.dbg_ren_peak, pk);
            atomic_max(&bridge.dbg_ren_peak_win_max, pk);
            g_atomic_int_inc(&bridge.dbg_ren_total_packets);
            if (pk <= 2)
                g_atomic_int_inc(&bridge.dbg_ren_quiet_packets);
        }

        IAudioRenderClient_ReleaseBuffer(bridge.render_iface, frames_available, 0);
    }

render_exit:
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
    return NULL;
}

/* ----- Start / stop ----- */

static void
bridge_stop_internal(void)
{
    gboolean was_running = bridge.running;

    g_atomic_int_set(&bridge.stop_request, 1);

    if (bridge.capture_client) IAudioClient_Stop(bridge.capture_client);
    if (bridge.render_client)  IAudioClient_Stop(bridge.render_client);

    if (bridge.capture_thread) { g_thread_join(bridge.capture_thread); bridge.capture_thread = NULL; }
    if (bridge.render_thread)  { g_thread_join(bridge.render_thread);  bridge.render_thread = NULL; }

    if (bridge.capture_iface) { IAudioCaptureClient_Release(bridge.capture_iface); bridge.capture_iface = NULL; }
    if (bridge.render_iface)  { IAudioRenderClient_Release(bridge.render_iface);   bridge.render_iface = NULL; }
    if (bridge.capture_volume){ ISimpleAudioVolume_Release(bridge.capture_volume); bridge.capture_volume = NULL; }
    if (bridge.render_volume) { ISimpleAudioVolume_Release(bridge.render_volume);  bridge.render_volume = NULL; }
    if (bridge.capture_client){ IAudioClient_Release(bridge.capture_client);       bridge.capture_client = NULL; }
    if (bridge.render_client) { IAudioClient_Release(bridge.render_client);        bridge.render_client = NULL; }
    if (bridge.capture_device){ IMMDevice_Release(bridge.capture_device);          bridge.capture_device = NULL; }
    if (bridge.render_device) { IMMDevice_Release(bridge.render_device);           bridge.render_device = NULL; }
    if (bridge.capture_event) { CloseHandle(bridge.capture_event);                 bridge.capture_event = NULL; }
    if (bridge.render_event)  { CloseHandle(bridge.render_event);                  bridge.render_event = NULL; }
    if (bridge.format)        { CoTaskMemFree(bridge.format);                      bridge.format = NULL; }
    if (bridge.capture_id_w)  { g_free(bridge.capture_id_w);                       bridge.capture_id_w = NULL; }
    if (bridge.render_id_w)   { g_free(bridge.render_id_w);                        bridge.render_id_w = NULL; }

    if (bridge.render_current)
    {
        g_byte_array_unref(bridge.render_current);
        bridge.render_current = NULL;
        bridge.render_current_offset = 0;
    }
    if (bridge.audio_queue)
    {
        queue_drain(bridge.audio_queue);
        g_async_queue_unref(bridge.audio_queue);
        bridge.audio_queue = NULL;
    }

    bridge.running = FALSE;
    g_atomic_int_set(&bridge.stop_request, 0);
    bridge.bytes_per_frame = 0;

    /* Don't clobber an ERROR status; otherwise reset to STOPPED. */
    if (bridge.status != AUDIO_BRIDGE_ERROR)
        bridge.status = AUDIO_BRIDGE_STOPPED;

    /* If the bridge was actually running and the radio is still connected,
       hand control of the volume back to the radio by syncing the current
       slider value. Skip on failed-start cleanup paths (was_running=FALSE)
       and on disconnect (tuner.thread becomes NULL). */
    if (was_running && tuner.thread)
    {
        gchar buffer[5];
        g_snprintf(buffer, sizeof(buffer), "Y%d", conf.volume);
        tuner_write(tuner.thread, buffer);
    }
}

static gboolean
bridge_start_internal(void)
{
    if (bridge.running) return TRUE;
    clear_error();

    if (!conf.audio_bridge_input_id || !*conf.audio_bridge_input_id ||
        !conf.audio_bridge_output_id || !*conf.audio_bridge_output_id)
    {
        set_error("No input or output device selected");
        return FALSE;
    }

    /* Reset debug counters at the start of every run. The restart count is
       preserved across auto-restarts (see request_restart). */
    g_atomic_int_set(&bridge.dbg_cap_events, 0);
    g_atomic_int_set(&bridge.dbg_cap_frames, 0);
    g_atomic_int_set(&bridge.dbg_cap_frames_silent, 0);
    g_atomic_int_set(&bridge.dbg_cap_last_ms, 0);
    g_atomic_int_set(&bridge.dbg_ren_events, 0);
    g_atomic_int_set(&bridge.dbg_ren_frames, 0);
    g_atomic_int_set(&bridge.dbg_ren_underruns, 0);
    g_atomic_int_set(&bridge.dbg_ren_last_ms, 0);
    g_atomic_int_set(&bridge.dbg_drift_stretch, 0);
    g_atomic_int_set(&bridge.dbg_drift_skip, 0);
    g_atomic_int_set(&bridge.dbg_cap_peak, 0);
    g_atomic_int_set(&bridge.dbg_ren_peak, 0);
    g_atomic_int_set(&bridge.dbg_start_ms,
                     (gint)(g_get_monotonic_time() / 1000));

    bridge.capture_device = open_device_by_id(conf.audio_bridge_input_id);
    if (!bridge.capture_device)
    {
        set_error("Could not open input device (was it removed?)");
        goto fail;
    }
    bridge.render_device = open_device_by_id(conf.audio_bridge_output_id);
    if (!bridge.render_device)
    {
        set_error("Could not open output device (was it removed?)");
        goto fail;
    }

    /* Cache wide-string IDs for hot-plug comparisons */
    bridge.capture_id_w = utf8_to_wide(conf.audio_bridge_input_id);
    bridge.render_id_w  = utf8_to_wide(conf.audio_bridge_output_id);

    HRESULT hr;
    hr = IMMDevice_Activate(bridge.capture_device, &BRIDGE_IID_IAudioClient,
                            CLSCTX_ALL, NULL, (void**)&bridge.capture_client);
    if (FAILED(hr)) { set_error("Activate(capture IAudioClient) failed (hr=0x%08lx)", (long)hr); goto fail; }

    hr = IMMDevice_Activate(bridge.render_device, &BRIDGE_IID_IAudioClient,
                            CLSCTX_ALL, NULL, (void**)&bridge.render_client);
    if (FAILED(hr)) { set_error("Activate(render IAudioClient) failed (hr=0x%08lx)", (long)hr); goto fail; }

    WAVEFORMATEX *cap_fmt = NULL;
    WAVEFORMATEX *ren_fmt = NULL;
    if (FAILED(IAudioClient_GetMixFormat(bridge.capture_client, &cap_fmt)))
    {
        set_error("GetMixFormat(capture) failed");
        goto fail;
    }
    if (FAILED(IAudioClient_GetMixFormat(bridge.render_client,  &ren_fmt)))
    {
        CoTaskMemFree(cap_fmt);
        set_error("GetMixFormat(render) failed");
        goto fail;
    }

    if (!formats_match(cap_fmt, ren_fmt))
    {
        set_error("Capture and render formats differ "
                  "(input: %u Hz / %u ch / %u bit, output: %u Hz / %u ch / %u bit). "
                  "Open Windows Sound settings and set both devices to the same format.",
                  (unsigned)cap_fmt->nSamplesPerSec, cap_fmt->nChannels, cap_fmt->wBitsPerSample,
                  (unsigned)ren_fmt->nSamplesPerSec, ren_fmt->nChannels, ren_fmt->wBitsPerSample);
        CoTaskMemFree(cap_fmt);
        CoTaskMemFree(ren_fmt);
        goto fail;
    }

    CoTaskMemFree(ren_fmt);
    bridge.format = cap_fmt;
    bridge.bytes_per_frame = bridge.format->nBlockAlign;

    REFERENCE_TIME duration = (REFERENCE_TIME)BRIDGE_BUFFER_MS * 10000;

    hr = IAudioClient_Initialize(bridge.capture_client,
                                 AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 duration, 0, bridge.format, NULL);
    if (FAILED(hr)) { set_error("capture Initialize failed (hr=0x%08lx)", (long)hr); goto fail; }

    hr = IAudioClient_Initialize(bridge.render_client,
                                 AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 duration, 0, bridge.format, NULL);
    if (FAILED(hr)) { set_error("render Initialize failed (hr=0x%08lx)", (long)hr); goto fail; }

    bridge.capture_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    bridge.render_event  = CreateEventW(NULL, FALSE, FALSE, NULL);
    IAudioClient_SetEventHandle(bridge.capture_client, bridge.capture_event);
    IAudioClient_SetEventHandle(bridge.render_client,  bridge.render_event);

    hr = IAudioClient_GetService(bridge.capture_client, &BRIDGE_IID_IAudioCaptureClient,
                                 (void**)&bridge.capture_iface);
    if (FAILED(hr)) { set_error("GetService(capture) failed (hr=0x%08lx)", (long)hr); goto fail; }

    /* Force the capture session's per-stream volume to unity. Windows
       remembers this per-app, so a previously-lowered xdr-gtk recording
       session would otherwise attenuate the bridge audio. */
    hr = IAudioClient_GetService(bridge.capture_client, &BRIDGE_IID_ISimpleAudioVolume,
                                 (void**)&bridge.capture_volume);
    if (SUCCEEDED(hr) && bridge.capture_volume)
    {
        ISimpleAudioVolume_SetMasterVolume(bridge.capture_volume, 1.0f, NULL);
        ISimpleAudioVolume_SetMute(bridge.capture_volume, FALSE, NULL);
    }
    else
    {
        bridge.capture_volume = NULL;
    }

    hr = IAudioClient_GetService(bridge.render_client, &BRIDGE_IID_IAudioRenderClient,
                                 (void**)&bridge.render_iface);
    if (FAILED(hr)) { set_error("GetService(render) failed (hr=0x%08lx)", (long)hr); goto fail; }

    /* Per-stream volume control. Failure is non-fatal: if for some reason
       the device doesn't expose ISimpleAudioVolume the bridge still works,
       just without volume slider control. */
    hr = IAudioClient_GetService(bridge.render_client, &BRIDGE_IID_ISimpleAudioVolume,
                                 (void**)&bridge.render_volume);
    if (FAILED(hr))
    {
        log_hr("GetService(ISimpleAudioVolume)", hr);
        bridge.render_volume = NULL;
    }
    else
    {
        gint v = conf.volume;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        ISimpleAudioVolume_SetMasterVolume(bridge.render_volume, (float)v / 100.0f, NULL);
        ISimpleAudioVolume_SetMute(bridge.render_volume, FALSE, NULL);
    }

    /* Create the audio queue (empty; capture will start pushing packets). */
    bridge.audio_queue = g_async_queue_new();
    bridge.render_current = NULL;
    bridge.render_current_offset = 0;

    g_atomic_int_set(&bridge.stop_request, 0);

    /* Start capture first so the queue has data ready when render asks. */
    hr = IAudioClient_Start(bridge.capture_client);
    if (FAILED(hr)) { set_error("capture Start failed (hr=0x%08lx)", (long)hr); goto fail; }

    bridge.capture_thread = g_thread_new("xdr-bridge-cap", capture_thread_fn, NULL);

    /* Brief pre-fill so render has something to play on its first wakeup.
       Wait until at least 10 packets (~100 ms) are queued, or up to 200 ms. */
    for (int i = 0; i < 200 && g_async_queue_length(bridge.audio_queue) < 10; i++)
        Sleep(1);

    hr = IAudioClient_Start(bridge.render_client);
    if (FAILED(hr)) { set_error("render Start failed (hr=0x%08lx)", (long)hr); goto fail; }

    bridge.render_thread = g_thread_new("xdr-bridge-ren", render_thread_fn, NULL);
    bridge.running = TRUE;
    bridge.status = AUDIO_BRIDGE_RUNNING;

    /* The slider now drives the bridge's per-stream volume, so the radio
       must stay at full output to give the capture path the cleanest
       signal. Send Y100 once on start; we restore the slider value to the
       radio when the bridge stops. */
    if (tuner.thread)
        tuner_write(tuner.thread, "Y100");

    g_message("%s started: %u Hz, %u ch, %u bit, queue max %d packets, wasapi %d ms",
              BRIDGE_LOG_PREFIX,
              (unsigned)bridge.format->nSamplesPerSec,
              bridge.format->nChannels,
              bridge.format->wBitsPerSample,
              BRIDGE_QUEUE_MAX, BRIDGE_BUFFER_MS);
    return TRUE;

fail:
    bridge_stop_internal();
    return FALSE;
}

void
audio_bridge_apply_state(void)
{
    gboolean want = conf.audio_bridge_enabled
                    && tuner.thread
                    && conf.connection_mode != 2  /* webserver delivers audio over WS */
                    && conf.audio_bridge_input_id
                    && conf.audio_bridge_output_id
                    && *conf.audio_bridge_input_id
                    && *conf.audio_bridge_output_id;

    if (want && !bridge.running)
    {
        /* Wipe any prior error message before attempting to start. */
        clear_error();
        bridge.status = AUDIO_BRIDGE_STOPPED;
        bridge_start_internal();
    }
    else if (!want && bridge.running)
    {
        bridge_stop_internal();
        clear_error();
    }
    else if (!want)
    {
        /* Bridge isn't supposed to be running and isn't — clear stale errors
           when the user has actively turned off the feature. */
        if (!conf.audio_bridge_enabled)
        {
            clear_error();
            bridge.status = AUDIO_BRIDGE_STOPPED;
        }
    }
}

gboolean
audio_bridge_is_running(void)
{
    return bridge.running;
}

void
audio_bridge_set_volume(gint volume_0_100)
{
    if (!bridge.running || !bridge.render_volume)
        return;
    if (volume_0_100 < 0) volume_0_100 = 0;
    if (volume_0_100 > 100) volume_0_100 = 100;
    ISimpleAudioVolume_SetMasterVolume(bridge.render_volume,
                                       (float)volume_0_100 / 100.0f,
                                       NULL);
}

void
audio_bridge_debug_get(audio_bridge_debug_snapshot_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->status     = bridge.status;
    out->last_error = bridge.last_error;

    out->cap_events        = g_atomic_int_get(&bridge.dbg_cap_events);
    out->cap_frames        = g_atomic_int_get(&bridge.dbg_cap_frames);
    out->cap_frames_silent = g_atomic_int_get(&bridge.dbg_cap_frames_silent);
    out->cap_last_ms       = g_atomic_int_get(&bridge.dbg_cap_last_ms);

    out->ren_events   = g_atomic_int_get(&bridge.dbg_ren_events);
    out->ren_frames   = g_atomic_int_get(&bridge.dbg_ren_frames);
    out->ren_underruns= g_atomic_int_get(&bridge.dbg_ren_underruns);
    out->ren_last_ms  = g_atomic_int_get(&bridge.dbg_ren_last_ms);

    out->drift_stretch = g_atomic_int_get(&bridge.dbg_drift_stretch);
    out->drift_skip    = g_atomic_int_get(&bridge.dbg_drift_skip);

    out->restarts = g_atomic_int_get(&bridge.dbg_restarts);

    out->start_ms = g_atomic_int_get(&bridge.dbg_start_ms);
    out->now_ms   = (gint)(g_get_monotonic_time() / 1000);

    /* "Ring" fields now report queue depth as a percentage of the
       drop-oldest threshold so the debug window still shows a meaningful
       number. Capacity = BRIDGE_QUEUE_MAX; fill = current depth. */
    out->ring_capacity = BRIDGE_QUEUE_MAX;
    out->ring_fill     = bridge.audio_queue ? g_async_queue_length(bridge.audio_queue) : 0;

    if (bridge.format)
    {
        out->format_sample_rate = (guint)bridge.format->nSamplesPerSec;
        out->format_channels    = bridge.format->nChannels;
        out->format_bits        = bridge.format->wBitsPerSample;
    }

    out->cap_peak = g_atomic_int_get(&bridge.dbg_cap_peak);
    out->ren_peak = g_atomic_int_get(&bridge.dbg_ren_peak);

    /* Read-and-reset the per-window tracking counters so each snapshot
       covers a fresh window. Tiny race with the worker threads is fine. */
    out->cap_peak_win_max      = g_atomic_int_get(&bridge.dbg_cap_peak_win_max);
    out->ren_peak_win_max      = g_atomic_int_get(&bridge.dbg_ren_peak_win_max);
    out->ring_peek_peak_win_max= g_atomic_int_get(&bridge.dbg_ring_peek_peak_win_max);
    out->cap_quiet_packets  = g_atomic_int_get(&bridge.dbg_cap_quiet_packets);
    out->cap_total_packets  = g_atomic_int_get(&bridge.dbg_cap_total_packets);
    out->ren_quiet_packets  = g_atomic_int_get(&bridge.dbg_ren_quiet_packets);
    out->ren_total_packets  = g_atomic_int_get(&bridge.dbg_ren_total_packets);
    g_atomic_int_set(&bridge.dbg_cap_peak_win_max, 0);
    g_atomic_int_set(&bridge.dbg_ren_peak_win_max, 0);
    g_atomic_int_set(&bridge.dbg_ring_peek_peak_win_max, 0);
    g_atomic_int_set(&bridge.dbg_cap_quiet_packets, 0);
    g_atomic_int_set(&bridge.dbg_cap_total_packets, 0);
    g_atomic_int_set(&bridge.dbg_ren_quiet_packets, 0);
    g_atomic_int_set(&bridge.dbg_ren_total_packets, 0);

    /* Live read-back of the render session's volume / mute so we can spot
       Windows (or another app) attenuating us behind our back. */
    out->render_volume_pct = -1;
    out->render_muted      = FALSE;
    if (bridge.running && bridge.render_volume)
    {
        float vol = 0.0f;
        if (SUCCEEDED(ISimpleAudioVolume_GetMasterVolume(bridge.render_volume, &vol)))
            out->render_volume_pct = (gint)(vol * 100.0f + 0.5f);
        BOOL muted = FALSE;
        if (SUCCEEDED(ISimpleAudioVolume_GetMute(bridge.render_volume, &muted)))
            out->render_muted = muted ? TRUE : FALSE;
    }
}

#else /* !G_OS_WIN32 */

void audio_bridge_init(void) {}
void audio_bridge_shutdown(void) {}
GList* audio_bridge_enumerate_devices(gboolean input) { (void)input; return NULL; }
void audio_bridge_device_list_free(GList *list) { g_list_free_full(list, g_free); }
void audio_bridge_apply_state(void) {}
gboolean audio_bridge_is_running(void) { return FALSE; }
audio_bridge_status_t audio_bridge_get_status(void) { return AUDIO_BRIDGE_STOPPED; }
const gchar* audio_bridge_get_last_error(void) { return NULL; }
void audio_bridge_set_volume(gint volume_0_100) { (void)volume_0_100; }
void audio_bridge_debug_get(audio_bridge_debug_snapshot_t *out) { if (out) memset(out, 0, sizeof(*out)); }

#endif
