/* uaos_http.h — shared HTTP/1.1 client for UAOS userspace tools
 *
 * Header-only, freestanding HTTP client built on uaos_socket.h, used by
 * the gnu: wget and curl ports so both share one implementation of URL
 * parsing, request building, response parsing (Content-Length, chunked
 * transfer coding, close-delimited bodies) and redirect resolution.
 *
 * Usage:
 *   UaosUrl u;  uaos_http_parse_url("http://host/path", &u);
 *   UaosHttp h; uaos_http_open(&h, &u, "GET", hdrs, n, NULL, 0, ua, 30000, 0);
 *   while ((n = uaos_http_read(&h, buf, sizeof buf)) > 0) consume(buf, n);
 *   uaos_http_close(&h);
 */

#ifndef UAOS_HTTP_H
#define UAOS_HTTP_H

#include <stdint.h>
#include <stddef.h>
#include "uaos_libc.h"
#include "uaos_socket.h"
#include "uaos_tls.h"

/* Error codes — uaos_http_open/uaos_http_read return these (< 0). */
#define UAOS_HERR_URL      -1   /* malformed URL */
#define UAOS_HERR_PROTO    -2   /* unsupported scheme */
#define UAOS_HERR_DNS      -3   /* hostname resolution failed */
#define UAOS_HERR_CONNECT  -4   /* TCP connect failed/refused */
#define UAOS_HERR_SEND     -5   /* send error / send timeout */
#define UAOS_HERR_RECV     -6   /* recv error / recv timeout */
#define UAOS_HERR_CLOSED   -7   /* connection closed mid-headers */
#define UAOS_HERR_HDR      -8   /* malformed HTTP response */
#define UAOS_HERR_NOMEM    -9   /* header block exceeds buffer */
#define UAOS_HERR_TLS      -10  /* TLS handshake/verification failed
                                   (detail in UaosHttp.tls_err) */

#define UAOS_HTTP_MAX_HEADERS  32
#define UAOS_HTTP_HDRBUF       4096
#define UAOS_HTTP_SOCKBUF      1460
#define UAOS_HTTP_PATH_MAX     512
#define UAOS_HTTP_HOST_MAX     128
#define UAOS_HTTP_MAX_REDIRECT 20

typedef struct {
    char     scheme[8];                      /* "http" / "https" */
    char     host[UAOS_HTTP_HOST_MAX];
    uint16_t port;                           /* resolved default port */
    char     path[UAOS_HTTP_PATH_MAX];       /* "/path?query" — always '/'-rooted */
    char     userinfo[96];                   /* "user:pass" from URL, or "" */
} UaosUrl;

typedef struct {
    char *name;
    char *value;
} UaosHdr;

typedef struct {
    int      sock;                           /* usock handle, -1 = closed */
    int      err;                            /* last UAOS_HERR_* */
    UaosUrl  url;
    int      status;                         /* e.g. 200 */
    char     status_line[160];               /* "HTTP/1.1 200 OK" */
    UaosHdr  hdrs[UAOS_HTTP_MAX_HEADERS];
    int      hdr_count;
    char     hdr_buf[UAOS_HTTP_HDRBUF];      /* header name/value storage */
    long     content_length;                 /* -1 = absent */
    int      chunked;
    /* socket read buffer shared by header and body phases */
    uint8_t  rbuf[UAOS_HTTP_SOCKBUF];
    int      rlen;
    int      ridx;
    int      reof;
    /* body accounting */
    long     body_left;                      /* Content-Length remaining */
    long     chunk_left;                     /* bytes left in current chunk */
    int      chunk_eof;                      /* terminal 0-chunk consumed */
    long     body_total;                     /* decoded bytes delivered */
    int      head_only;                      /* -I/HEAD: no body follows */
    int      tls;                            /* https: transport is TLS */
    int      tls_err;                        /* BearSSL/UAOS_TLS_E_* detail */
} UaosHttp;

static inline const char *uaos_http_errstr(int err)
{
    switch (err) {
    case UAOS_HERR_URL:     return "malformed URL";
    case UAOS_HERR_PROTO:   return "unsupported URL scheme (https needs TLS)";
    case UAOS_HERR_DNS:     return "could not resolve host";
    case UAOS_HERR_CONNECT: return "connection failed";
    case UAOS_HERR_SEND:    return "send failed";
    case UAOS_HERR_RECV:    return "receive failed";
    case UAOS_HERR_CLOSED:  return "connection closed by peer";
    case UAOS_HERR_HDR:     return "malformed HTTP response";
    case UAOS_HERR_NOMEM:   return "response headers too large";
    case UAOS_HERR_TLS:     return "TLS handshake/verification failed";
    }
    return "unknown error";
}

/* -------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

static inline int uaos_http_atoi_digit(int c)
{
    return (c >= '0' && c <= '9') ? c - '0' : -1;
}

static inline int uaos_http_hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline void uaos_http_dec(long v, char *out)
{
    char tmp[24];
    int n = 0;
    unsigned long u = (v < 0) ? (unsigned long)(-(v + 1)) + 1ul : (unsigned long)v;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    int i = 0;
    if (v < 0) out[i++] = '-';
    while (n) out[i++] = tmp[--n];
    out[i] = '\0';
}

/* base64-encode `len` bytes into out (NUL-terminated). Returns length. */
static inline int uaos_http_b64(const uint8_t *data, int len, char *out, int max)
{
    static const char B64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int w = 0;
    for (int i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        int rem = len - i;
        if (rem > 1) v |= (uint32_t)data[i + 1] << 8;
        if (rem > 2) v |= (uint32_t)data[i + 2];
        if (w + 4 >= max) break;
        out[w++] = B64[(v >> 18) & 63];
        out[w++] = B64[(v >> 12) & 63];
        out[w++] = (rem > 1) ? B64[(v >> 6) & 63] : '=';
        out[w++] = (rem > 2) ? B64[v & 63] : '=';
    }
    out[w] = '\0';
    return w;
}

/* Format "Authorization: Basic <b64(userinfo)>" for a -u USER:PASS string.
 * Returns out on success, NULL if userinfo is too long. */
static inline const char *uaos_http_basic_auth(const char *userinfo,
                                               char *out, int max)
{
    static const char PRE[] = "Authorization: Basic ";
    int w = 0;
    for (const char *s = PRE; *s; s++) out[w++] = *s;
    w += uaos_http_b64((const uint8_t *)userinfo, (int)uaos_strlen(userinfo),
                       out + w, max - w);
    out[w] = '\0';
    return out;
}

static inline int uaos_http_stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        int d = uaos_tolower((uint8_t)*a) - uaos_tolower((uint8_t)*b);
        if (d) return d;
        a++; b++;
    }
    return uaos_tolower((uint8_t)*a) - uaos_tolower((uint8_t)*b);
}

/* n-byte case-insensitive compare — returns 0 when equal. */
static inline int uaos_http_strnicmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int d = uaos_tolower((uint8_t)a[i]) - uaos_tolower((uint8_t)b[i]);
        if (d) return d;
        if (!a[i]) return 0;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * URL parsing
 *
 * Accepts "http://[user[:pass]@]host[:port][/path][?query]" and a bare
 * "host/path" (scheme defaults to http).  https parses OK but
 * uaos_http_open rejects it until TLS support lands.
 * ------------------------------------------------------------------------- */
static inline int uaos_http_parse_url(const char *url, UaosUrl *out)
{
    uaos_memset(out, 0, sizeof(*out));
    out->port = 80;
    uaos_strcpy(out->scheme, "http");
    out->path[0] = '/';
    out->path[1] = '\0';

    const char *p = url;
    const char *colon = uaos_strchr(url, ':');
    if (colon && colon > url) {
        /* "scheme:" — must be followed by "//" */
        int slen = (int)(colon - url);
        if (slen >= (int)sizeof(out->scheme))
            return UAOS_HERR_URL;
        for (int i = 0; i < slen; i++) {
            int c = uaos_tolower((uint8_t)url[i]);
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '+' || c == '-' || c == '.'))
                return UAOS_HERR_URL;
            out->scheme[i] = (char)c;
        }
        out->scheme[slen] = '\0';
        p = colon + 1;
        if (p[0] != '/' || p[1] != '/')
            return UAOS_HERR_URL;
        p += 2;
    }

    if (uaos_strcmp(out->scheme, "http") &&
        uaos_strcmp(out->scheme, "https"))
        return UAOS_HERR_PROTO;
    if (!uaos_strcmp(out->scheme, "https"))
        out->port = 443;

    /* authority = up to '/', '?', or end */
    const char *auth_end = p;
    while (*auth_end && *auth_end != '/' && *auth_end != '?')
        auth_end++;
    const char *path_start = auth_end;

    /* userinfo@host[:port] */
    const char *at = NULL;
    for (const char *q = p; q < auth_end; q++)
        if (*q == '@') at = q;
    if (at) {
        int ulen = (int)(at - p);
        if (ulen >= (int)sizeof(out->userinfo))
            return UAOS_HERR_URL;
        uaos_memcpy(out->userinfo, p, (size_t)ulen);
        out->userinfo[ulen] = '\0';
        p = at + 1;
    }

    const char *pcolon = NULL;
    for (const char *q = p; q < auth_end; q++)
        if (*q == ':') pcolon = q;
    int hlen = (int)((pcolon ? pcolon : auth_end) - p);
    if (hlen <= 0 || hlen >= (int)sizeof(out->host))
        return UAOS_HERR_URL;
    uaos_memcpy(out->host, p, (size_t)hlen);
    out->host[hlen] = '\0';

    if (pcolon) {
        long port = 0;
        for (const char *q = pcolon + 1; q < auth_end; q++) {
            int d = uaos_http_atoi_digit((uint8_t)*q);
            if (d < 0) return UAOS_HERR_URL;
            port = port * 10 + d;
            if (port > 65535) return UAOS_HERR_URL;
        }
        if (port > 0) out->port = (uint16_t)port;
    }

    /* path (+ optional ?query): keep verbatim from '/' or '?' */
    if (*path_start) {
        int i = 0;
        if (*path_start == '?')
            out->path[i++] = '/';           /* bare query -> "/?q" */
        const char *q = path_start;
        while (*q && i < (int)sizeof(out->path) - 1)
            out->path[i++] = *q++;
        out->path[i] = '\0';
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Redirect resolution — resolve Location against the request URL.
 * Handles absolute URLs, "//host/...", absolute "/path", bare "?query",
 * and relative paths merged into the base's directory with ../ ./ removed.
 * ------------------------------------------------------------------------- */
static inline int uaos_http_is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

/* Remove "." and ".." segments from a '/'-rooted path (in place). */
static inline void uaos_http_norm_path(char *path)
{
    char out[UAOS_HTTP_PATH_MAX];
    int w = 0;
    const char *r = path;
    while (*r) {
        const char *cs = r;
        while (*r && *r != '/') r++;
        int cl = (int)(r - cs);
        if (*r) r++;
        if (cl == 0) continue;
        if (cl == 1 && cs[0] == '.') continue;
        if (cl == 2 && cs[0] == '.' && cs[1] == '.') {
            while (w > 0 && out[w - 1] != '/') w--;
            if (w > 0) w--;
            continue;
        }
        if (w > 0 || cs[0] != '/') {
            if (w > 0) out[w++] = '/';
        }
        for (int i = 0; i < cl && w < (int)sizeof(out) - 1; i++)
            out[w++] = cs[i];
    }
    if (w == 0) out[w++] = '/';
    out[w] = '\0';
    uaos_strcpy(path, out);
}

static inline int uaos_http_resolve_redirect(const UaosUrl *base,
                                             const char *loc,
                                             UaosUrl *out)
{
    /* Absolute URL? */
    const char *colon = uaos_strchr(loc, ':');
    if (colon && colon > loc) {
        int has_slashes = (colon[1] == '/' && colon[2] == '/');
        int scheme_ok = 1;
        for (const char *q = loc; q < colon; q++) {
            int c = uaos_tolower((uint8_t)*q);
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '+' || c == '-' || c == '.'))
                scheme_ok = 0;
        }
        if (scheme_ok && has_slashes)
            return uaos_http_parse_url(loc, out);
    }

    *out = *base;
    if (loc[0] == '/' && loc[1] == '/') {
        /* scheme-relative: "//host/path" */
        char tmp[UAOS_HTTP_PATH_MAX + UAOS_HTTP_HOST_MAX + 16];
        int i = 0;
        for (const char *s = base->scheme; *s && i < (int)sizeof(tmp) - 2; )
            tmp[i++] = *s++;
        tmp[i++] = ':';
        uaos_strncpy(tmp + i, loc, sizeof(tmp) - (size_t)i);
        return uaos_http_parse_url(tmp, out);
    }
    if (loc[0] == '/') {
        uaos_strncpy(out->path, loc, sizeof(out->path));
        return 0;
    }
    if (loc[0] == '?') {
        /* replace query of current path */
        int i = 0;
        while (base->path[i] && base->path[i] != '?' &&
               i < (int)sizeof(out->path) - 1) {
            out->path[i] = base->path[i];
            i++;
        }
        int j = 0;
        while (loc[j] && i < (int)sizeof(out->path) - 1)
            out->path[i++] = loc[j++];
        out->path[i] = '\0';
        return 0;
    }

    /* Relative: merge into the directory portion of the base path. */
    char merged[UAOS_HTTP_PATH_MAX];
    int last_slash = -1;
    for (int k = 0; base->path[k]; k++) {
        merged[k] = base->path[k];
        if (base->path[k] == '/') last_slash = k;
        if (base->path[k] == '?') break;
    }
    int w = (last_slash >= 0) ? last_slash + 1 : 0;
    int j = 0;
    while (loc[j] && w < (int)sizeof(merged) - 1)
        merged[w++] = loc[j++];
    merged[w] = '\0';
    uaos_strncpy(out->path, merged, sizeof(out->path));
    uaos_http_norm_path(out->path);
    return 0;
}

/* -------------------------------------------------------------------------
 * Transport — plain socket or TLS depending on h->tls
 * ------------------------------------------------------------------------- */
static inline long uaos_http_send(UaosHttp *h, const void *buf, long len)
{
    return h->tls ? (long)uaos_tls_send(buf, (int)len)
                  : uaos_sock_send(h->sock, buf, len);
}

static inline long uaos_http_recv(UaosHttp *h, void *buf, long len)
{
    return h->tls ? (long)uaos_tls_recv(buf, (int)len)
                  : uaos_sock_recv(h->sock, buf, len);
}

/* -------------------------------------------------------------------------
 * Socket buffered reader
 * ------------------------------------------------------------------------- */
static inline int uaos_http_getc(UaosHttp *h)
{
    if (h->ridx >= h->rlen) {
        if (h->reof) return -1;
        long n = uaos_http_recv(h, h->rbuf, sizeof(h->rbuf));
        if (n <= 0) { h->reof = 1; return -1; }
        h->rlen = (int)n;
        h->ridx = 0;
    }
    return h->rbuf[h->ridx++];
}

/* -------------------------------------------------------------------------
 * Response header parsing
 * ------------------------------------------------------------------------- */
static inline int uaos_http_readline(UaosHttp *h, char *line, int max)
{
    int i = 0;
    for (;;) {
        int c = uaos_http_getc(h);
        if (c < 0)
            return (i > 0) ? i : -1;
        if (c == '\r')
            continue;
        if (c == '\n') {
            line[i] = '\0';
            return i;
        }
        if (i < max - 1)
            line[i++] = (char)c;
    }
}

static inline int uaos_http_parse_headers(UaosHttp *h)
{
    char line[512];
    int n = uaos_http_readline(h, line, sizeof(line));
    if (n < 0)
        return UAOS_HERR_CLOSED;
    if (uaos_strncmp(line, "HTTP/", 5) != 0)
        return UAOS_HERR_HDR;
    uaos_strncpy(h->status_line, line, sizeof(h->status_line));

    /* status code follows the first space */
    h->status = 0;
    const char *sp = uaos_strchr(line, ' ');
    if (sp) {
        sp++;
        while (*sp >= '0' && *sp <= '9')
            h->status = h->status * 10 + (*sp++ - '0');
    }
    if (h->status < 100)
        return UAOS_HERR_HDR;

    int hdr_off = 0;
    for (;;) {
        n = uaos_http_readline(h, line, sizeof(line));
        if (n < 0)
            return UAOS_HERR_CLOSED;
        if (n == 0)
            break;                      /* empty line: end of headers */
        if (h->hdr_count >= UAOS_HTTP_MAX_HEADERS)
            continue;                   /* drop extras, keep reading */
        const char *c = uaos_strchr(line, ':');
        if (!c)
            continue;                   /* tolerate malformed lines */
        int nlen = (int)(c - line);
        const char *v = c + 1;
        while (*v == ' ' || *v == '\t') v++;
        int vlen = (int)uaos_strlen(v);
        if (hdr_off + nlen + vlen + 2 > UAOS_HTTP_HDRBUF)
            return UAOS_HERR_NOMEM;
        char *nbuf = h->hdr_buf + hdr_off;
        uaos_memcpy(nbuf, line, (size_t)nlen);
        nbuf[nlen] = '\0';
        uaos_strcpy(nbuf + nlen + 1, v);
        h->hdrs[h->hdr_count].name  = nbuf;
        h->hdrs[h->hdr_count].value = nbuf + nlen + 1;
        h->hdr_count++;
        hdr_off += nlen + vlen + 2;
    }

    h->content_length = -1;
    h->chunked = 0;
    for (int i = 0; i < h->hdr_count; i++) {
        if (!uaos_http_stricmp(h->hdrs[i].name, "Content-Length")) {
            long v = 0;
            for (const char *q = h->hdrs[i].value; *q >= '0' && *q <= '9'; q++)
                v = v * 10 + (*q - '0');
            h->content_length = v;
        } else if (!uaos_http_stricmp(h->hdrs[i].name, "Transfer-Encoding")) {
            const char *v = h->hdrs[i].value;
            /* "chunked" may appear in a list; substring match is enough
             * for our purposes. */
            while (*v) {
                if (!uaos_strncmp(v, "chunked", 7)) { h->chunked = 1; break; }
                if (!uaos_strncmp(v, "Chunked", 7)) { h->chunked = 1; break; }
                v++;
            }
        }
    }
    if (h->chunked)
        h->content_length = -1;
    if (h->head_only || h->status == 204 || h->status == 304 ||
        (h->status >= 100 && h->status < 200)) {
        h->content_length = 0;
        h->chunked = 0;
    }
    h->body_left = h->content_length;
    h->chunk_left = 0;
    h->chunk_eof = 0;
    h->body_total = 0;
    return 0;
}

/* -------------------------------------------------------------------------
 * Open — resolve, connect, send request, parse response head.
 *
 * headers: optional array of complete "Name: value" strings (already
 * formatted); body/body_len: optional request entity (Content-Length is
 * added automatically when body_len > 0).  timeout_ms covers the whole
 * connect+request+response-head phase via the socket timeouts.
 * ------------------------------------------------------------------------- */
static inline int uaos_http_open(UaosHttp *h, const UaosUrl *url,
                                 const char *method,
                                 const char *const *headers, int n_headers,
                                 const void *body, long body_len,
                                 const char *user_agent,
                                 uint32_t timeout_ms, int insecure)
{
    uaos_memset(h, 0, sizeof(*h));
    h->sock = -1;
    h->url = *url;
    h->head_only = (method && !uaos_strcmp(method, "HEAD"));

    int want_tls = !uaos_strcmp(url->scheme, "https");
    if (!want_tls && uaos_strcmp(url->scheme, "http"))
        return (h->err = UAOS_HERR_PROTO);

    uint32_t ip;
    long rc = uaos_resolve_host(url->host, &ip, timeout_ms ? timeout_ms : 5000);
    if (rc < 0)
        return (h->err = UAOS_HERR_DNS);

    long sock = uaos_socket(UAOS_SOCK_STREAM);
    if (sock < 0)
        return (h->err = (int)sock == UAOS_ENETDOWN ? UAOS_HERR_CONNECT
                                                   : UAOS_HERR_CONNECT);
    h->sock = (int)sock;
    if (timeout_ms) {
        uaos_sock_setopt(h->sock, UAOS_SOCKOPT_CONNECT_TIMEOUT, timeout_ms);
        uaos_sock_setopt(h->sock, UAOS_SOCKOPT_RECV_TIMEOUT, timeout_ms);
        uaos_sock_setopt(h->sock, UAOS_SOCKOPT_SEND_TIMEOUT, timeout_ms);
    }

    rc = uaos_connect(h->sock, ip, url->port);
    if (rc < 0) {
        uaos_sock_close(h->sock);
        h->sock = -1;
        return (h->err = (rc == UAOS_ETIMEDOUT) ? UAOS_HERR_CONNECT
                                                : UAOS_HERR_CONNECT);
    }

    if (want_tls) {
        rc = uaos_tls_handshake(h->sock, url->host, insecure);
        /* handshake returns 0 or the engine code — BearSSL BR_ERR_*
         * values are positive, so test rc != 0, not rc < 0. */
        if (rc != 0) {
            h->tls_err = (int)rc;
            uaos_sock_close(h->sock);
            h->sock = -1;
            return (h->err = UAOS_HERR_TLS);
        }
        h->tls = 1;
    }

    /* Build the request head. */
    char req[2048];
    int w = 0;
    #define REQ_APPEND(str) do { \
        const char *_s = (str); \
        while (*_s && w < (int)sizeof(req) - 2) req[w++] = *_s++; \
    } while (0)

    REQ_APPEND(method ? method : "GET");
    REQ_APPEND(" ");
    REQ_APPEND(url->path[0] ? url->path : "/");
    REQ_APPEND(" HTTP/1.1\r\nHost: ");
    REQ_APPEND(url->host);
    if (!((url->port == 80 && !uaos_strcmp(url->scheme, "http")) ||
          (url->port == 443 && !uaos_strcmp(url->scheme, "https")))) {
        char pb[8];
        uaos_http_dec(url->port, pb);
        REQ_APPEND(":");
        REQ_APPEND(pb);
    }
    REQ_APPEND("\r\nUser-Agent: ");
    REQ_APPEND(user_agent ? user_agent : "UAOS-http/1.0");
    REQ_APPEND("\r\nConnection: close\r\n");
    REQ_APPEND("Accept: */*\r\n");
    if (url->userinfo[0]) {
        /* Basic auth: base64("user:pass") — RFC 7617. */
        char auth[192];
        uaos_http_basic_auth(url->userinfo, auth, sizeof(auth));
        REQ_APPEND(auth);
        REQ_APPEND("\r\n");
    }
    for (int i = 0; i < n_headers; i++) {
        REQ_APPEND(headers[i]);
        REQ_APPEND("\r\n");
    }
    if (body_len > 0) {
        char cl[16];
        uaos_http_dec(body_len, cl);
        REQ_APPEND("Content-Length: ");
        REQ_APPEND(cl);
        REQ_APPEND("\r\n");
    }
    REQ_APPEND("\r\n");
    #undef REQ_APPEND
    req[w] = '\0';

    rc = uaos_http_send(h, req, w);
    if (rc < 0)
        goto send_fail;
    if (body_len > 0) {
        rc = uaos_http_send(h, body, body_len);
        if (rc < 0)
            goto send_fail;
    }

    rc = uaos_http_parse_headers(h);
    if (rc < 0) {
        if (h->tls) uaos_tls_close(); else uaos_sock_close(h->sock);
        h->sock = -1;
        return (h->err = (int)rc);
    }
    return 0;

send_fail:
    if (h->tls) uaos_tls_close(); else uaos_sock_close(h->sock);
    h->sock = -1;
    return (h->err = UAOS_HERR_SEND);
}

/* -------------------------------------------------------------------------
 * Body reader — decodes chunked TE, Content-Length, or close-delimited.
 * Returns bytes written to buf, 0 at EOF, < 0 UAOS_HERR_* on error.
 * ------------------------------------------------------------------------- */
static inline long uaos_http_read(UaosHttp *h, void *buf, long max)
{
    uint8_t *out = (uint8_t *)buf;
    long got = 0;

    if (h->sock < 0 || max <= 0)
        return UAOS_HERR_CLOSED;
    if (h->content_length == 0 && !h->chunked)
        return 0;                       /* HEAD/204/304: no body */

    while (got < max) {
        /* Decide how many body bytes are available to produce now. */
        long want;
        if (h->chunked) {
            if (h->chunk_eof)
                break;
            if (h->chunk_left == 0) {
                /* Read the next chunk-size line.  Blank lines are
                 * skipped: each data chunk is followed by a CRLF that
                 * arrives as an empty line on this reader, and the first
                 * chunk starts right after the header terminator. */
                char line[128];
                int n;
                for (;;) {
                    n = uaos_http_readline(h, line, sizeof(line));
                    if (n < 0)
                        return got ? got : UAOS_HERR_CLOSED;
                    if (n > 0)
                        break;
                }
                long sz = 0;
                int any = 0;
                for (int i = 0; i < n && line[i] != ';'; i++) {
                    int hv = uaos_http_hexval((uint8_t)line[i]);
                    if (hv < 0) break;
                    sz = sz * 16 + hv;
                    any = 1;
                }
                if (!any)
                    return got ? got : UAOS_HERR_HDR;
                if (sz == 0) {
                    /* terminal chunk — drain trailer lines to blank */
                    for (;;) {
                        int tn = uaos_http_readline(h, line, sizeof(line));
                        if (tn <= 0) break;
                    }
                    h->chunk_eof = 1;
                    break;
                }
                h->chunk_left = sz;
            }
            want = h->chunk_left;
            if (want > max - got) want = max - got;
        } else {
            if (h->body_left == 0)
                break;
            want = max - got;
            if (h->body_left > 0 && want > h->body_left)
                want = h->body_left;
        }

        /* Serve from the buffered reader first; fall through to a direct
         * socket recv once it is drained so large transfers don't pay a
         * copy per byte. */
        long n;
        if (h->ridx < h->rlen) {
            n = h->rlen - h->ridx;
            if (n > want) n = want;
            uaos_memcpy(out + got, h->rbuf + h->ridx, (size_t)n);
            h->ridx += (int)n;
        } else {
            n = uaos_http_recv(h, out + got, want);
            if (n < 0)
                return got ? got : (long)UAOS_HERR_RECV;
            if (n == 0)
                break;                  /* close-delimited EOF */
        }
        got += n;
        h->body_total += n;
        if (h->chunked)
            h->chunk_left -= n;
        else if (h->body_left > 0)
            h->body_left -= n;
    }
    return got;
}

/* Header lookup — first match, case-insensitive.  NULL when absent. */
static inline const char *uaos_http_header(const UaosHttp *h, const char *name)
{
    for (int i = 0; i < h->hdr_count; i++)
        if (!uaos_http_stricmp(h->hdrs[i].name, name))
            return h->hdrs[i].value;
    return NULL;
}

static inline void uaos_http_close(UaosHttp *h)
{
    if (h->sock >= 0) {
        if (h->tls)
            uaos_tls_close();
        else
            uaos_sock_close(h->sock);
        h->sock = -1;
        h->tls = 0;
    }
}

/* Convenience: basename of a URL path for -O/--remote-name style output.
 * Returns a pointer into url->path (or "index.html" for a bare "/"). */
static inline const char *uaos_http_url_filename(const UaosUrl *u)
{
    const char *p = u->path;
    const char *last = NULL;
    for (const char *q = p; *q && *q != '?'; q++)
        if (*q == '/' && q[1] && q[1] != '?')
            last = q + 1;
    if (!last || !*last)
        return "index.html";
    return last;
}

#endif /* UAOS_HTTP_H */
