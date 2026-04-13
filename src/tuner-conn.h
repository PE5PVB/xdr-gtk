#ifndef XDR_TUNER_CONN_H
#define XDR_TUNER_CONN_H
#include <gtk/gtk.h>
#ifndef G_OS_WIN32
#define closesocket(x) close(x)
#endif

#define MODE_SERIAL 0
#define MODE_SOCKET 1

#define CONN_SUCCESS 0

#define CONN_SERIAL_FAIL_OPEN    -1
#define CONN_SERIAL_FAIL_PARM_R  -2
#define CONN_SERIAL_FAIL_PARM_W  -3
#define CONN_SERIAL_FAIL_SPEED   -4

#define CONN_SOCKET_STATE_UNDEF 100
#define CONN_SOCKET_STATE_RESOLV  1
#define CONN_SOCKET_STATE_CONN    2
#define CONN_SOCKET_STATE_AUTH    3
#define CONN_SOCKET_FAIL_RESOLV  -1
#define CONN_SOCKET_FAIL_CONN    -2
#define CONN_SOCKET_FAIL_AUTH    -3
#define CONN_SOCKET_FAIL_WRITE   -4
#define CONN_SOCKET_FAIL_URL     -5
#define CONN_SOCKET_FAIL_UPGRADE -6
#define CONN_SOCKET_FAIL_TLS     -7

#define SOCKET_SALT_LEN 16
#define SOCKET_AUTH_TIMEOUT 5

typedef struct conn
{
    gchar* hostname;
    gchar* port;
    gchar* password;
    gchar* ws_path;          /* WebSocket path (e.g. /xdrgtk), NULL for raw TCP */
    gboolean websocket;      /* TRUE when this is an fm-dx-webserver connection */
    gboolean tls;            /* TRUE for wss:// / https:// URLs */
    volatile gboolean canceled;
    gint socketfd;
    gpointer stream;         /* GIOStream* (owned) — set on successful WS connect */
    gpointer cancel;         /* GCancellable* (owned) */
    gint state;
} conn_t;

gint tuner_open_serial(const gchar*, gintptr*);
gpointer tuner_open_socket(gpointer);
gpointer tuner_open_websocket(gpointer);
const gchar* tuner_ws_last_error(void);

conn_t* conn_new(const gchar*, const gchar* port, const gchar*);
conn_t* conn_new_ws(const gchar* url, const gchar* password);
void conn_free(conn_t*);

/* WebSocket client helpers shared with audio_stream. Work on GIOStream /
   GCancellable pairs as set up by g_socket_client_connect_to_host. */
gboolean stream_write_all(GOutputStream *out, const guchar *buf, gsize len, GCancellable *cancel);
gboolean stream_read_all(GInputStream *in, guchar *buf, gsize len, GCancellable *cancel);
gboolean ws_parse_url(const gchar *url, gchar **out_host, gchar **out_port,
                      gchar **out_path, gboolean *out_tls);
gboolean ws_send_masked_frame(gpointer iostream, GCancellable *cancel,
                              guchar opcode, const guchar *payload, gsize len);
gint     ws_read_frame(gpointer iostream, GCancellable *cancel,
                       guchar **out_buf, gsize *out_len, guchar *out_op);
gchar*   ws_read_http_response(GIOStream *stream, GCancellable *cancel);
#endif
