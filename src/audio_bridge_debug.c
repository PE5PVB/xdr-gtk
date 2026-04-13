#include "audio_bridge_debug.h"
#include "audio_bridge.h"
#include <glib.h>
#include <stdio.h>

#ifdef G_OS_WIN32
#include <windows.h>

#define POLL_INTERVAL_MS 500

static gboolean console_ready = FALSE;
static guint    poll_id       = 0;
static audio_bridge_debug_snapshot_t prev;
static gboolean prev_valid    = FALSE;
static gint64   prev_wall_ms  = 0;

/* Prevent accidental xdr-gtk kill when the user closes the console X. */
static BOOL WINAPI
console_ctrl_handler(DWORD type)
{
    if (type == CTRL_CLOSE_EVENT ||
        type == CTRL_C_EVENT ||
        type == CTRL_BREAK_EVENT ||
        type == CTRL_LOGOFF_EVENT ||
        type == CTRL_SHUTDOWN_EVENT)
        return TRUE;
    return FALSE;
}

static void
ensure_console(void)
{
    if (console_ready) return;

    if (!AllocConsole())
    {
        /* Already had one (launched from cmd). That's fine. */
    }
    SetConsoleTitleA("xdr-gtk audio bridge debug");
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    /* Rebind CRT stdio to the freshly allocated console. */
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    console_ready = TRUE;

    printf("\n=== xdr-gtk audio bridge debug ===\n");
    printf("Polling every %d ms. Closing this window is ignored (safe).\n", POLL_INTERVAL_MS);
    printf("Columns: st(atus) cap(ture) ren(der) ring vol mute drift restarts\n");
    printf("Interesting transitions appear on their own '*** …' lines.\n\n");
    fflush(stdout);
}

static const char*
status_short(audio_bridge_status_t s)
{
    switch (s)
    {
        case AUDIO_BRIDGE_RUNNING: return "RUN";
        case AUDIO_BRIDGE_ERROR:   return "ERR";
        case AUDIO_BRIDGE_STOPPED:
        default:                   return "STP";
    }
}

static void
print_stamp(void)
{
    GDateTime *dt = g_date_time_new_now_local();
    gchar *stamp = g_date_time_format(dt, "%H:%M:%S");
    g_date_time_unref(dt);
    printf("[%s] ", stamp);
    g_free(stamp);
}

static gboolean
poll_tick(gpointer data)
{
    (void)data;
    audio_bridge_debug_snapshot_t s;
    audio_bridge_debug_get(&s);
    gint64 wall = g_get_monotonic_time() / 1000;

    gint cap_silent_pct = -1;  /* -1 means no data this tick */
    gint under_delta = 0;
    if (prev_valid)
    {
        gint dframes = s.cap_frames - prev.cap_frames;
        gint dsilent = s.cap_frames_silent - prev.cap_frames_silent;
        if (dframes > 0)
            cap_silent_pct = (gint)((gint64)dsilent * 100 / dframes);
        under_delta = s.ren_underruns - prev.ren_underruns;
    }

    gint ring_pct = (s.ring_capacity > 0)
                    ? (gint)((gint64)s.ring_fill * 100 / s.ring_capacity)
                    : 0;
    gint cap_age = s.cap_last_ms ? (s.now_ms - s.cap_last_ms) : -1;
    gint ren_age = s.ren_last_ms ? (s.now_ms - s.ren_last_ms) : -1;

    gint cap_quiet_pct = (s.cap_total_packets > 0)
        ? (gint)((gint64)s.cap_quiet_packets * 100 / s.cap_total_packets)
        : 0;
    gint ren_quiet_pct = (s.ren_total_packets > 0)
        ? (gint)((gint64)s.ren_quiet_packets * 100 / s.ren_total_packets)
        : 0;

    print_stamp();
    printf("%s cap(pk=%3d%% max=%3d%% q=%3d%% sil=%3d%%)"
           " ring=%3d%% peek=%3d%%"
           " ren(pk=%3d%% max=%3d%% q=%3d%% und+%d)"
           " vol=%3d%% mute=%s skip=%d rst=%d",
           status_short(s.status),
           s.cap_peak, s.cap_peak_win_max,
           cap_quiet_pct,
           cap_silent_pct < 0 ? 0 : cap_silent_pct,
           ring_pct, s.ring_peek_peak_win_max,
           s.ren_peak, s.ren_peak_win_max,
           ren_quiet_pct, under_delta,
           s.render_volume_pct, s.render_muted ? "YES" : "no",
           s.drift_skip, s.restarts);
    if (s.last_error)
        printf(" err=\"%s\"", s.last_error);
    printf("\n");

    if (prev_valid)
    {
        if (prev.cap_peak > 2 && s.cap_peak == 0)
        {
            print_stamp();
            printf("  *** CAPTURE PEAK -> 0 (input is now silent)\n");
        }
        else if (prev.cap_peak == 0 && s.cap_peak > 2)
        {
            print_stamp();
            printf("  *** capture peak recovered (%d%%)\n", s.cap_peak);
        }

        if (prev.ren_peak > 2 && s.ren_peak == 0)
        {
            print_stamp();
            printf("  *** RENDER PEAK -> 0 (output is now silent)\n");
        }
        else if (prev.ren_peak == 0 && s.ren_peak > 2)
        {
            print_stamp();
            printf("  *** render peak recovered (%d%%)\n", s.ren_peak);
        }

        if (s.status != prev.status)
        {
            print_stamp();
            printf("  *** status %s -> %s\n",
                   status_short(prev.status), status_short(s.status));
        }

        if (s.restarts > prev.restarts)
        {
            print_stamp();
            printf("  *** AUTO-RESTART #%d (err=%s)\n",
                   s.restarts, s.last_error ? s.last_error : "none");
        }

        if (s.render_volume_pct != prev.render_volume_pct &&
            s.render_volume_pct >= 0 && prev.render_volume_pct >= 0)
        {
            print_stamp();
            printf("  *** render session volume: %d%% -> %d%%\n",
                   prev.render_volume_pct, s.render_volume_pct);
        }

        if (s.render_muted != prev.render_muted)
        {
            print_stamp();
            printf("  *** render session %s\n",
                   s.render_muted ? "MUTED (by Windows or another app)" : "unmuted");
        }

        /* Capture silent-fraction transition — key for the "suddenly silent"
           case where WASAPI keeps reporting events but flags everything
           AUDCLNT_BUFFERFLAGS_SILENT. */
        {
            static gint prev_cur_pct = -1;
            gint cur_pct = cap_silent_pct;
            if (cur_pct >= 0 && prev_cur_pct >= 0)
            {
                if (prev_cur_pct < 50 && cur_pct >= 50)
                {
                    print_stamp();
                    printf("  *** capture SILENT FRACTION jumped %d%% -> %d%% "
                           "(WASAPI is flagging packets as silent)\n",
                           prev_cur_pct, cur_pct);
                }
                else if (prev_cur_pct >= 50 && cur_pct < 50)
                {
                    print_stamp();
                    printf("  *** capture silent fraction recovered %d%% -> %d%%\n",
                           prev_cur_pct, cur_pct);
                }
            }
            if (cur_pct >= 0) prev_cur_pct = cur_pct;
        }

        /* Stall detection on data flow even when events still come. */
        if (s.status == AUDIO_BRIDGE_RUNNING)
        {
            if (cap_age >= 0 && cap_age > 500 &&
                prev.cap_last_ms && (prev.now_ms - prev.cap_last_ms) <= 500)
            {
                print_stamp();
                printf("  *** capture: no event for > 500 ms\n");
            }
            if (ren_age >= 0 && ren_age > 500 &&
                prev.ren_last_ms && (prev.now_ms - prev.ren_last_ms) <= 500)
            {
                print_stamp();
                printf("  *** render: no event for > 500 ms\n");
            }
        }
    }

    fflush(stdout);

    prev = s;
    prev_valid = TRUE;
    prev_wall_ms = wall;

    return G_SOURCE_CONTINUE;
}

void
audio_bridge_debug_show(void)
{
    ensure_console();
    if (!poll_id)
    {
        prev_valid = FALSE;
        poll_id = g_timeout_add(POLL_INTERVAL_MS, poll_tick, NULL);
        poll_tick(NULL);
    }
}

#else /* !G_OS_WIN32 */

void audio_bridge_debug_show(void) {}

#endif
