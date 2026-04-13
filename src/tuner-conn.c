#include <string.h>
#include <stdlib.h>
#include <gio/gio.h>
#include "ui-connect.h"
#include "tuner-conn.h"
#include "tuner.h"
#ifdef G_OS_WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include "win32.h"
#else
#include <fcntl.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <dirent.h>
#include <netinet/tcp.h>

#endif

#define SOCKET_TCP_KEEPCNT    2
#define SOCKET_TCP_KEEPINTVL 10
#define SOCKET_TCP_KEEPIDLE  30

gint
tuner_open_serial(const gchar *serial_port,
                  gintptr     *fd)
{
    gchar path[100];
#ifdef G_OS_WIN32
    HANDLE serial;
    DCB dcbSerialParams = {0};

    g_snprintf(path, sizeof(path), "\\\\.\\%s", serial_port);
    serial = CreateFile(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if(serial == INVALID_HANDLE_VALUE)
        return CONN_SERIAL_FAIL_OPEN;
    if(!GetCommState(serial, &dcbSerialParams))
    {
        CloseHandle(serial);
        return CONN_SERIAL_FAIL_PARM_R;
    }
    dcbSerialParams.BaudRate = CBR_115200;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;
    if(!SetCommState(serial, &dcbSerialParams))
    {
        CloseHandle(serial);
        return CONN_SERIAL_FAIL_PARM_W;
    }
    *fd = (gintptr)serial;
#else
    struct termios options;
    g_snprintf(path, sizeof(path), "/dev/%s", serial_port);
    *fd = open(path, O_RDWR | O_NOCTTY | O_NDELAY);
    if(*fd < 0)
        return CONN_SERIAL_FAIL_OPEN;
    fcntl(*fd, F_SETFL, 0);
    tcflush(*fd, TCIOFLUSH);
    if(tcgetattr(*fd, &options))
    {
        close(*fd);
        return CONN_SERIAL_FAIL_PARM_R;
    }
    if(cfsetispeed(&options, B115200) || cfsetospeed(&options, B115200))
    {
        close(*fd);
        return CONN_SERIAL_FAIL_SPEED;
    }
    options.c_iflag &= ~(BRKINT | ICRNL | IXON | IMAXBEL);
    options.c_iflag |= IGNBRK;
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG | IEXTEN | ECHOK | ECHOCTL | ECHOKE);
    options.c_oflag &= ~(OPOST | ONLCR);
    options.c_oflag |= NOFLSH;
    options.c_cflag |= CS8;
    options.c_cflag &= ~(CRTSCTS);
    if(tcsetattr(*fd, TCSANOW, &options))
    {
        close(*fd);
        return CONN_SERIAL_FAIL_PARM_W;
    }
#endif
    return CONN_SUCCESS;
}

gpointer
tuner_open_socket(gpointer ptr)
{
    conn_t *data = (conn_t*)ptr;
    struct addrinfo hints = {0}, *result;
    struct timeval timeout = {0};
    fd_set input;
    gchar salt[SOCKET_SALT_LEN+1], msg[42];
    GChecksum *sha1;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    /* Resolve the hostname */
    data->state = CONN_SOCKET_STATE_RESOLV;
    g_idle_add(connection_socket_callback_info, data);
    if(getaddrinfo(data->hostname, data->port, &hints, &result))
    {
        data->state = CONN_SOCKET_FAIL_RESOLV;
        g_idle_add(connection_socket_callback, data);
        return NULL;
    }

    if(data->canceled)
    {
        freeaddrinfo(result);
        g_idle_add(connection_socket_callback, data);
        return NULL;
    }

    data->state = CONN_SOCKET_STATE_CONN;
    g_idle_add(connection_socket_callback_info, data);
    data->socketfd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if(connect(data->socketfd, result->ai_addr, result->ai_addrlen) < 0 || data->canceled)
    {
        closesocket(data->socketfd);
        freeaddrinfo(result);
        data->state = CONN_SOCKET_FAIL_CONN;
        g_idle_add(connection_socket_callback, data);
        return NULL;
    }
    freeaddrinfo(result);

    data->state = CONN_SOCKET_STATE_AUTH;
    g_idle_add(connection_socket_callback_info, data);
    FD_ZERO(&input);
    FD_SET(data->socketfd, &input);
    timeout.tv_sec = SOCKET_AUTH_TIMEOUT;
    /* Wait SOCKET_AUTH_TIMEOUT seconds for the salt */
    if(select(data->socketfd+1, &input, NULL, NULL, &timeout) <= 0 || data->canceled)
    {
        closesocket(data->socketfd);
        data->state = CONN_SOCKET_FAIL_AUTH;
        g_idle_add(connection_socket_callback, data);
        return NULL;
    }

    /* Receive SOCKET_SALT_LENGTH+1 bytes (with \n) */
    if(recv(data->socketfd, salt, SOCKET_SALT_LEN+1, 0) != SOCKET_SALT_LEN+1 || data->canceled)
    {
        closesocket(data->socketfd);
        data->state = CONN_SOCKET_FAIL_AUTH;
        g_idle_add(connection_socket_callback, data);
        return NULL;
    }

    /* Calculate the SHA1 checksum of salt and password concatenation */
    sha1 = g_checksum_new(G_CHECKSUM_SHA1);
    g_checksum_update(sha1, (guchar*)salt, SOCKET_SALT_LEN);
    if(data->password && strlen(data->password))
    {
        g_checksum_update(sha1, (guchar*)data->password, strlen(data->password));
    }
    g_snprintf(msg, sizeof(msg), "%s\n", g_checksum_get_string(sha1));
    g_checksum_free(sha1);

    /* Send the hash */
    if(!tuner_write_socket(data->socketfd, msg, strlen(msg)) || data->canceled)
    {
        closesocket(data->socketfd);
        data->state = CONN_SOCKET_FAIL_WRITE;
        g_idle_add(connection_socket_callback, data);
        return NULL;
    }

#ifdef G_OS_WIN32
    DWORD ret;
    struct tcp_keepalive ka =
    {
        .onoff = 1,
        .keepaliveinterval = SOCKET_TCP_KEEPINTVL * SOCKET_TCP_KEEPCNT * 1000 / 10,
        .keepalivetime = SOCKET_TCP_KEEPIDLE * 1000
    };
    WSAIoctl(data->socketfd, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), NULL, 0, &ret, NULL, NULL);
#else

#if !defined(SOL_TCP) && defined(IPPROTO_TCP)
#define SOL_TCP IPPROTO_TCP
#endif
#if !defined(TCP_KEEPIDLE) && defined(TCP_KEEPALIVE)
#define TCP_KEEPIDLE TCP_KEEPALIVE
#endif

    gint opt = 1;
    if(setsockopt(data->socketfd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) >= 0)
    {
        opt = SOCKET_TCP_KEEPCNT;
        setsockopt(data->socketfd, IPPROTO_TCP, TCP_KEEPCNT, &opt, sizeof(opt));

        opt = SOCKET_TCP_KEEPINTVL;
        setsockopt(data->socketfd, IPPROTO_TCP, TCP_KEEPINTVL, &opt, sizeof(opt));

        opt = SOCKET_TCP_KEEPIDLE;
        setsockopt(data->socketfd, IPPROTO_TCP, TCP_KEEPIDLE, &opt, sizeof(opt));
    }
#endif

    data->state = CONN_SUCCESS;
    g_idle_add(connection_socket_callback, data);
    return NULL;
}

conn_t*
conn_new(const gchar *hostname,
         const gchar *port,
         const gchar *password)
{
    conn_t *ptr = g_new(conn_t, 1);
    ptr->hostname = g_strdup(hostname);
    ptr->port = g_strdup(port);
    ptr->password = g_strdup(password);
    ptr->ws_path = NULL;
    ptr->websocket = FALSE;
    ptr->canceled = FALSE;
    ptr->state = CONN_SOCKET_STATE_UNDEF;
    return ptr;
}

/* Parse ws:// / http:// URL into host, port, path.
   Accepts: [ws|http|wss|https]://host[:port][/path]
   Returns FALSE on parse failure, sets *tls=TRUE for wss/https. */
static gboolean
ws_parse_url(const gchar *url,
             gchar **out_host,
             gchar **out_port,
             gchar **out_path,
             gboolean *out_tls)
{
    const gchar *p = url;
    gboolean tls = FALSE;
    if (g_ascii_strncasecmp(p, "wss://", 6) == 0)      { tls = TRUE;  p += 6; }
    else if (g_ascii_strncasecmp(p, "https://", 8) == 0){ tls = TRUE;  p += 8; }
    else if (g_ascii_strncasecmp(p, "ws://", 5) == 0)  { p += 5; }
    else if (g_ascii_strncasecmp(p, "http://", 7) == 0){ p += 7; }
    else { /* bare host[:port][/path] — assume ws */ }

    const gchar *slash = strchr(p, '/');
    const gchar *hostend = slash ? slash : p + strlen(p);
    const gchar *colon = NULL;
    for (const gchar *q = p; q < hostend; q++) if (*q == ':') { colon = q; break; }

    if (colon)
    {
        *out_host = g_strndup(p, colon - p);
        *out_port = g_strndup(colon + 1, hostend - colon - 1);
    }
    else
    {
        *out_host = g_strndup(p, hostend - p);
        *out_port = g_strdup(tls ? "443" : "80");
    }
    if (slash && slash[0] == '/' && slash[1] != '\0')
        *out_path = g_strdup(slash);
    else
        *out_path = g_strdup("/xdrgtk");

    *out_tls = tls;
    if (!**out_host)
    {
        g_free(*out_host); g_free(*out_port); g_free(*out_path);
        return FALSE;
    }
    return TRUE;
}

conn_t*
conn_new_ws(const gchar *url,
            const gchar *password)
{
    conn_t *ptr = g_new(conn_t, 1);
    gchar *host = NULL, *port = NULL, *path = NULL;
    gboolean tls = FALSE;
    if (!ws_parse_url(url, &host, &port, &path, &tls))
    {
        ptr->hostname = g_strdup("");
        ptr->port = g_strdup("");
        ptr->ws_path = g_strdup("/xdrgtk");
    }
    else
    {
        ptr->hostname = host;
        ptr->port = port;
        ptr->ws_path = path;
    }
    ptr->password = g_strdup(password ? password : "");
    ptr->websocket = TRUE;
    ptr->tls = tls;
    ptr->stream = NULL;
    ptr->cancel = NULL;
    ptr->canceled = FALSE;
    ptr->state = CONN_SOCKET_STATE_UNDEF;
    return ptr;
}

void
conn_free(conn_t *ptr)
{
    g_free(ptr->hostname);
    g_free(ptr->port);
    g_free(ptr->password);
    g_free(ptr->ws_path);
    if (ptr->stream)
    {
        g_io_stream_close(G_IO_STREAM(ptr->stream), NULL, NULL);
        g_object_unref(ptr->stream);
    }
    if (ptr->cancel)
        g_object_unref(ptr->cancel);
    g_free(ptr);
}

/* Last error message captured by tuner_open_websocket, shown in the
   connection dialog so the user can see why TLS/connect failed. */
static gchar *ws_last_err = NULL;
const gchar* tuner_ws_last_error(void) { return ws_last_err; }
static void ws_set_err(const gchar *s)
{
    g_free(ws_last_err);
    ws_last_err = s ? g_strdup(s) : NULL;
}

/* ---------------- WebSocket client helpers (GIO stream based) ---------------- */

static gboolean
stream_write_all(GOutputStream *out,
                 const guchar  *buf,
                 gsize          len,
                 GCancellable  *cancel)
{
    gsize sent = 0;
    while (sent < len)
    {
        gssize n = g_output_stream_write(out, buf + sent, len - sent, cancel, NULL);
        if (n <= 0) return FALSE;
        sent += n;
    }
    return TRUE;
}

static gboolean
stream_read_all(GInputStream *in,
                guchar       *buf,
                gsize         len,
                GCancellable *cancel)
{
    gsize got = 0;
    while (got < len)
    {
        gssize n = g_input_stream_read(in, buf + got, len - got, cancel, NULL);
        if (n <= 0) return FALSE;
        got += n;
    }
    return TRUE;
}

/* Send a client WebSocket frame (always masked per RFC 6455). */
gboolean
ws_send_masked_frame(gpointer  iostream,
                     GCancellable *cancel,
                     guchar    opcode,
                     const guchar *payload,
                     gsize     len)
{
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(iostream));
    guchar hdr[14];
    gint hlen = 0;
    hdr[hlen++] = 0x80 | (opcode & 0x0F);
    if (len < 126)
    {
        hdr[hlen++] = 0x80 | (guchar)len;
    }
    else if (len < 65536)
    {
        hdr[hlen++] = 0x80 | 126;
        hdr[hlen++] = (len >> 8) & 0xFF;
        hdr[hlen++] = len & 0xFF;
    }
    else
    {
        hdr[hlen++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) hdr[hlen++] = (guchar)((guint64)len >> (i * 8)) & 0xFF;
    }
    guchar mask[4];
    guint32 r = (guint32)g_random_int();
    mask[0] = r & 0xFF; mask[1] = (r>>8)&0xFF; mask[2] = (r>>16)&0xFF; mask[3] = (r>>24)&0xFF;
    hdr[hlen++] = mask[0]; hdr[hlen++] = mask[1]; hdr[hlen++] = mask[2]; hdr[hlen++] = mask[3];
    if (!stream_write_all(out, hdr, hlen, cancel)) return FALSE;
    if (len > 0)
    {
        guchar *m = g_malloc(len);
        for (gsize i = 0; i < len; i++) m[i] = payload[i] ^ mask[i % 4];
        gboolean ok = stream_write_all(out, m, len, cancel);
        g_free(m);
        if (!ok) return FALSE;
    }
    return TRUE;
}

/* Read one complete WebSocket frame. Returns:
    1 : data frame stored in *out_buf (length *out_len), opcode in *out_op
    0 : control frame handled internally (ping->pong), caller should retry
   -1 : error / close frame
   Caller owns *out_buf (g_free). */
gint
ws_read_frame(gpointer     iostream,
              GCancellable *cancel,
              guchar      **out_buf,
              gsize        *out_len,
              guchar       *out_op)
{
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(iostream));
    guchar hdr[2];
    if (!stream_read_all(in, hdr, 2, cancel)) return -1;
    guchar opcode = hdr[0] & 0x0F;
    gboolean masked = (hdr[1] & 0x80) != 0;
    guint64 len = hdr[1] & 0x7F;
    if (len == 126)
    {
        guchar ext[2];
        if (!stream_read_all(in, ext, 2, cancel)) return -1;
        len = ((guint64)ext[0] << 8) | ext[1];
    }
    else if (len == 127)
    {
        guchar ext[8];
        if (!stream_read_all(in, ext, 8, cancel)) return -1;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }
    guchar mask[4] = {0};
    if (masked)
    {
        if (!stream_read_all(in, mask, 4, cancel)) return -1;
    }
    guchar *payload = NULL;
    if (len > 0)
    {
        if (len > (1u << 20)) return -1; /* sanity: reject >1 MB */
        payload = g_malloc(len);
        if (!stream_read_all(in, payload, len, cancel)) { g_free(payload); return -1; }
        if (masked)
            for (guint64 i = 0; i < len; i++) payload[i] ^= mask[i % 4];
    }

    if (opcode == 0x9) /* ping -> pong */
    {
        ws_send_masked_frame(iostream, cancel, 0xA, payload, len);
        g_free(payload);
        return 0;
    }
    if (opcode == 0x8) /* close */
    {
        g_free(payload);
        return -1;
    }
    if (opcode == 0xA) /* pong — ignore */
    {
        g_free(payload);
        return 0;
    }
    *out_buf = payload;
    *out_len = (gsize)len;
    *out_op = opcode;
    return 1;
}

/* Read HTTP headers terminated by CRLFCRLF. Returns allocated string or NULL. */
static gchar*
ws_read_http_response(GIOStream *stream, GCancellable *cancel)
{
    GInputStream *in = g_io_stream_get_input_stream(stream);
    GString *s = g_string_new(NULL);
    guchar ch;
    gsize end_match = 0;
    const guchar term[4] = { '\r', '\n', '\r', '\n' };
    while (s->len < 8192)
    {
        if (!stream_read_all(in, &ch, 1, cancel))
        {
            g_string_free(s, TRUE);
            return NULL;
        }
        g_string_append_c(s, (gchar)ch);
        if (ch == term[end_match]) end_match++;
        else end_match = (ch == '\r') ? 1 : 0;
        if (end_match == 4) return g_string_free(s, FALSE);
    }
    g_string_free(s, TRUE);
    return NULL;
}

gpointer
tuner_open_websocket(gpointer ptr)
{
    conn_t *data = (conn_t*)ptr;
    GChecksum *sha1;
    gchar msg[128];
    GError *err = NULL;

    if (!data->hostname || !*data->hostname || !data->port || !*data->port)
    {
        data->state = CONN_SOCKET_FAIL_URL;
        g_idle_add(connection_socket_callback, data);
        return NULL;
    }

    /* A single GCancellable drives TLS + read/write; on cancel we call
       g_cancellable_cancel() from the main loop via tuner_thread_cancel. */
    GCancellable *cancel = g_cancellable_new();
    data->cancel = cancel;

    data->state = CONN_SOCKET_STATE_RESOLV;
    g_idle_add(connection_socket_callback_info, data);

    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_tls(client, data->tls);
    /* Let the backend validate the cert normally. */
    g_socket_client_set_timeout(client, 15);

    data->state = CONN_SOCKET_STATE_CONN;
    g_idle_add(connection_socket_callback_info, data);

    GSocketConnection *sconn = g_socket_client_connect_to_host(
        client, data->hostname, (guint16)atoi(data->port), cancel, &err);
    g_object_unref(client);
    if (!sconn || data->canceled)
    {
        if (err)
        {
            /* Detect TLS backend missing: GIO reports this as
               G_IO_ERROR_NOT_SUPPORTED with a "TLS support is not
               available" message when glib-networking isn't installed. */
            gboolean is_tls = (err->domain == G_TLS_ERROR) ||
                              (err->message && strstr(err->message, "TLS"));
            g_print("tuner_open_websocket: connect failed: %s (domain=%s code=%d)\n",
                    err->message, g_quark_to_string(err->domain), err->code);
            ws_set_err(err->message);
            g_error_free(err);
            data->state = is_tls ? CONN_SOCKET_FAIL_TLS : CONN_SOCKET_FAIL_CONN;
        }
        else
        {
            ws_set_err(NULL);
            data->state = CONN_SOCKET_FAIL_CONN;
        }
        if (sconn) g_object_unref(sconn);
        g_idle_add(connection_socket_callback, data);
        return NULL;
    }

    GIOStream *stream = G_IO_STREAM(sconn);
    data->stream = stream;

    data->state = CONN_SOCKET_STATE_AUTH;
    g_idle_add(connection_socket_callback_info, data);

    /* Build HTTP Upgrade request */
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
        "User-Agent: xdr-gtk\r\n"
        "\r\n",
        data->ws_path ? data->ws_path : "/xdrgtk",
        data->hostname, data->port, key_b64);
    g_free(key_b64);

    GOutputStream *out = g_io_stream_get_output_stream(stream);
    if (!stream_write_all(out, (const guchar*)req, strlen(req), cancel) || data->canceled)
    {
        g_free(req);
        data->state = CONN_SOCKET_FAIL_WRITE;
        g_idle_add(connection_socket_callback, data);
        return NULL;
    }
    g_free(req);

    gchar *resp = ws_read_http_response(stream, cancel);
    if (!resp)
    {
        data->state = CONN_SOCKET_FAIL_UPGRADE;
        g_idle_add(connection_socket_callback, data);
        return NULL;
    }
    gboolean ok = (strstr(resp, " 101 ") != NULL);
    if (!ok)
        g_print("tuner_open_websocket: HTTP upgrade rejected:\n%s\n", resp);
    g_free(resp);
    if (!ok)
    {
        data->state = CONN_SOCKET_FAIL_UPGRADE;
        g_idle_add(connection_socket_callback, data);
        return NULL;
    }

    /* Read the salt frame */
    guchar *frame = NULL; gsize flen = 0; guchar fop = 0;
    gint r;
    do {
        r = ws_read_frame(stream, cancel, &frame, &flen, &fop);
    } while (r == 0);
    if (r < 0 || flen < SOCKET_SALT_LEN)
    {
        g_free(frame);
        data->state = CONN_SOCKET_FAIL_AUTH;
        g_idle_add(connection_socket_callback, data);
        return NULL;
    }

    sha1 = g_checksum_new(G_CHECKSUM_SHA1);
    g_checksum_update(sha1, frame, SOCKET_SALT_LEN);
    if (data->password && strlen(data->password))
        g_checksum_update(sha1, (guchar*)data->password, strlen(data->password));
    g_snprintf(msg, sizeof(msg), "%s\n", g_checksum_get_string(sha1));
    g_checksum_free(sha1);
    g_free(frame);

    if (!ws_send_masked_frame(stream, cancel, 0x1, (const guchar*)msg, strlen(msg)) || data->canceled)
    {
        data->state = CONN_SOCKET_FAIL_WRITE;
        g_idle_add(connection_socket_callback, data);
        return NULL;
    }

    data->state = CONN_SUCCESS;
    g_idle_add(connection_socket_callback, data);
    return NULL;
}

