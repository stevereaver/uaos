/* uaos_tls.h — TLS client transport for UAOS userspace, built on the
 * vendored BearSSL subset (system/bearssl/) over uaos_socket.h.
 *
 * Header-only like the rest of libuaos: each tool that links
 * libbearssl.a includes this once.  A single process-wide TLS context
 * lives in .bss — wget/curl are single-connection tools, so this is
 * deliberate (the engine alone needs ~33KB of record buffers).
 *
 * Also provides the freestanding libc shims the BearSSL objects
 * reference (memcpy/memset/memmove/memcmp/strlen/time) and stubs
 * br_prng_seeder_system() so ssl_engine.o does not pull in the
 * /dev/urandom seeder; entropy comes from SYSCALL_GETRANDOM instead.
 *
 * Verification: by default the x509_minimal engine validates against
 * the generated anchor table in system/bearssl/ta_roots.c and the
 * system clock (SYSCALL_TIME → ntp epoch).  insecure=1 installs an
 * accept-all verifier for curl -k / wget --no-check-certificate.
 */
#ifndef UAOS_TLS_H
#define UAOS_TLS_H

#include <stdint.h>
#include <stddef.h>
#include "bearssl.h"
#include "uaos_syscall.h"
#include "uaos_socket.h"
#include "uaos_libc.h"

#ifndef UAOS_TLS_HAVE_TIME_T
#define UAOS_TLS_HAVE_TIME_T
typedef long time_t;
#endif

/* ------------------------------------------------------------------ */
/* Freestanding shims for vendored BearSSL objects                     */
/* ------------------------------------------------------------------ */
void *memcpy(void *d, const void *s, size_t n)
{
    char *a = (char *)d;
    const char *b = (const char *)s;
    while (n--)
        *a++ = *b++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    char *a = (char *)d;
    const char *b = (const char *)s;
    if (a < b) {
        while (n--)
            *a++ = *b++;
    } else {
        a += n;
        b += n;
        while (n--)
            *--a = *--b;
    }
    return d;
}

void *memset(void *d, int c, size_t n)
{
    char *a = (char *)d;
    while (n--)
        *a++ = (char)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n--) {
        if (*x != *y)
            return *x - *y;
        x++;
        y++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* x509_minimal calls time() only if set_time was never called; we always
 * set it, so this is a safety net that still reflects the real clock. */
time_t time(time_t *t)
{
    time_t v = (time_t)uaos_time();
    if (t)
        *t = v;
    return v;
}

/* No OS RNG device exists; entropy is injected via
 * br_ssl_engine_inject_entropy() from SYSCALL_GETRANDOM, which marks the
 * HMAC-DRBG properly seeded before the handshake starts. */
br_prng_seeder br_prng_seeder_system(const char **name)
{
    (void)name;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Accept-all X.509 verifier for insecure mode                         */
/* ------------------------------------------------------------------ */
/* Accept-all verifier: decodes the end-entity cert so the engine can
 * use its public key for SKE signature checks, but performs no
 * chain/name/date validation.  The decoder's pkey_data lives in this
 * context, so the returned pkey pointers stay valid. */
typedef struct {
    const br_x509_class    *vtable;
    br_x509_decoder_context dc;
    int                     cert_num;
    int                     save;         /* decoding EE cert this pass */
    int                     have_pkey;
} uaos_x509_insecure_context;

static void uaos_xi_start_chain(const br_x509_class **ctx,
                                const char *server_name)
{
    uaos_x509_insecure_context *x =
        (uaos_x509_insecure_context *)(void *)ctx;
    (void)server_name;
    x->cert_num = 0;
    x->save = 0;
    x->have_pkey = 0;
}

static void uaos_xi_start_cert(const br_x509_class **ctx, uint32_t len)
{
    uaos_x509_insecure_context *x =
        (uaos_x509_insecure_context *)(void *)ctx;
    (void)len;
    if (x->cert_num == 0) {
        x->save = 1;
        br_x509_decoder_init(&x->dc, NULL, NULL);
    }
}

static void uaos_xi_append(const br_x509_class **ctx,
                           const unsigned char *buf, size_t len)
{
    uaos_x509_insecure_context *x =
        (uaos_x509_insecure_context *)(void *)ctx;
    if (x->save)
        br_x509_decoder_push(&x->dc, buf, len);
}

static void uaos_xi_end_cert(const br_x509_class **ctx)
{
    uaos_x509_insecure_context *x =
        (uaos_x509_insecure_context *)(void *)ctx;
    if (x->cert_num == 0 && x->save)
        x->have_pkey = (br_x509_decoder_last_error(&x->dc) == 0 &&
                        br_x509_decoder_get_pkey(&x->dc) != NULL);
    x->cert_num++;
    x->save = 0;
}

static unsigned uaos_xi_end_chain(const br_x509_class **ctx)
{
    (void)ctx;
    return 0;
}

static const br_x509_pkey *uaos_xi_get_pkey(
    const br_x509_class *const *ctx, unsigned *usages)
{
    const uaos_x509_insecure_context *x =
        (const uaos_x509_insecure_context *)(const void *)ctx;
    if (usages)
        *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    if (!x->have_pkey)
        return NULL;
    /* Key bytes live in x->dc.pkey_data (part of this context). */
    return br_x509_decoder_get_pkey((br_x509_decoder_context *)
                                    (void *)&x->dc);
}

static const br_x509_class uaos_x509_insecure_vtable = {
    sizeof(uaos_x509_insecure_context),
    uaos_xi_start_chain,
    uaos_xi_start_cert,
    uaos_xi_append,
    uaos_xi_end_cert,
    uaos_xi_end_chain,
    uaos_xi_get_pkey
};

/* ------------------------------------------------------------------ */
/* TLS client context (process-wide singleton)                         */
/* ------------------------------------------------------------------ */
typedef struct {
    int                          fd;        /* underlying usock, -1 closed */
    int                          ready;     /* handshake completed */
    int                          insecure;
    int                          err;       /* UAOS_TLS_E_* or BR_ERR_* */
    int                          xerr;      /* xc.err detail (verify mode) */
    unsigned                     diag;      /* engine state at failure */
    br_ssl_client_context        cc;
    br_x509_minimal_context      xc;
    uaos_x509_insecure_context   xi;
    unsigned char                iobuf[BR_SSL_BUFSIZE_BIDI];
} uaos_tls_t;

/* error codes (positive engine errors pass through unmodified) */
#define UAOS_TLS_E_NOTIME   -1   /* clock unset: cannot check cert dates */
#define UAOS_TLS_E_SEED     -2   /* SYSCALL_GETRANDOM returned no bytes */
#define UAOS_TLS_E_IO       -3   /* transport error/close mid-operation */
#define UAOS_TLS_E_ARGS     -4

static uaos_tls_t uaos_tls;

extern const br_x509_trust_anchor uaos_ta_roots[];
extern const size_t uaos_ta_roots_num;

/* Engine pump: move pending records over the socket until control
 * should return to the caller.
 *
 *  - RECVAPP always returns (decrypted data ready for the caller).
 *  - SENDAPP returns when app_ret is set (callers that want to send).
 *    Callers waiting for inbound data pass app_ret=0 so a transient
 *    SENDAPP+RECVREC state still services the inbound record.
 *  - RECVREC waits on the socket only when block_in is set; otherwise
 *    the pump returns so the caller can decide.  This matters once the
 *    handshake is done: the engine advertises RECVREC whenever buffer
 *    space exists, even when the peer has nothing to send.
 *
 * Returns 0, <0 on failure (t->err). */
static int uaos_tls_pump(uaos_tls_t *t, int block_in, int app_ret)
{
    for (;;) {
        unsigned st = br_ssl_engine_current_state(&t->cc.eng);

        if (st & BR_SSL_CLOSED) {
            int e = (int)br_ssl_engine_last_error(&t->cc.eng);
            t->diag = 0x100 | st;
            t->err = e ? e : UAOS_TLS_E_IO;
            return -1;
        }
        if (st & BR_SSL_SENDREC) {
            size_t len;
            unsigned char *buf = br_ssl_engine_sendrec_buf(&t->cc.eng,
                                                           &len);
            long w = uaos_sock_send(t->fd, buf, (int)len);
            if (w <= 0) {
                t->diag = 0x200 | st;
                t->err = UAOS_TLS_E_IO;
                return -1;
            }
            br_ssl_engine_sendrec_ack(&t->cc.eng, (size_t)w);
            continue;
        }
        if (st & BR_SSL_RECVAPP)
            return 0;
        if (app_ret && (st & BR_SSL_SENDAPP))
            return 0;
        if (st & BR_SSL_RECVREC) {
            if (!block_in)
                return 0;
            size_t len;
            unsigned char *buf = br_ssl_engine_recvrec_buf(&t->cc.eng,
                                                           &len);
            long r = uaos_sock_recv(t->fd, buf, (int)len);
            if (r <= 0) {   /* <0 error; 0 = TCP EOF mid-record */
                t->diag = 0x300 | st | ((unsigned)(-r) << 8);
                t->err = UAOS_TLS_E_IO;
                return -1;
            }
            br_ssl_engine_recvrec_ack(&t->cc.eng, (size_t)r);
            continue;
        }
        return 0;
    }
}

/* Wrap an already-connected socket in TLS and run the handshake.
 * host is used for SNI and (in secure mode) certificate name checking.
 * Returns 0 on success, <0 on failure (t->err set). */
static int uaos_tls_handshake(int fd, const char *host, int insecure)
{
    uaos_tls_t *t = &uaos_tls;
    uint8_t seed[64];

    memset(t, 0, sizeof(*t));
    t->fd = fd;
    t->insecure = insecure;

    if (insecure) {
        br_ssl_client_init_full(&t->cc, &t->xc, uaos_ta_roots,
                                uaos_ta_roots_num);
        t->xi.vtable = &uaos_x509_insecure_vtable;
        br_ssl_engine_set_x509(&t->cc.eng, &t->xi.vtable);
    } else {
        long epoch = uaos_time();
        if (epoch <= 0) {
            t->err = UAOS_TLS_E_NOTIME;
            return t->err;
        }
        br_ssl_client_init_full(&t->cc, &t->xc, uaos_ta_roots,
                                uaos_ta_roots_num);
        br_x509_minimal_set_time(&t->xc,
            (uint32_t)(epoch / 86400) + 719528u,
            (uint32_t)(epoch % 86400));
    }

    br_ssl_engine_set_buffer(&t->cc.eng, t->iobuf, sizeof(t->iobuf), 1);

    /* br_ssl_client_reset() runs br_ssl_engine_init_rand(), which fails
     * unless the HMAC-DRBG is already seeded — inject entropy first. */
    if (uaos_getrandom(seed, sizeof(seed)) != (long)sizeof(seed)) {
        t->err = UAOS_TLS_E_SEED;
        return t->err;
    }
    br_ssl_engine_inject_entropy(&t->cc.eng, seed, sizeof(seed));
    memset(seed, 0, sizeof(seed));

    if (!br_ssl_client_reset(&t->cc, host, 0)) {
        t->err = UAOS_TLS_E_ARGS;
        return t->err;
    }

    /* Handshake is complete as soon as the engine offers the app-data
     * channel (BR_SSL_SENDAPP / RECVAPP).  At that point RECVREC is
     * still set — the peer simply has nothing to send yet — so the
     * done check must precede the pump or the recv would deadlock. */
    for (;;) {
        unsigned st = br_ssl_engine_current_state(&t->cc.eng);
        if (st & BR_SSL_CLOSED) {
            int e = (int)br_ssl_engine_last_error(&t->cc.eng);
            t->diag = 0x400 | st;
            if (e >= BR_ERR_X509_INVALID_VALUE &&
                e <= BR_ERR_X509_NOT_TRUSTED)
                t->xerr = t->xc.err;
            t->err = e ? e : UAOS_TLS_E_IO;
            return t->err;
        }
        if (st & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) {
            t->ready = 1;
            return 0;
        }
        if (uaos_tls_pump(t, 1, 1) < 0)
            return t->err;
        if ((br_ssl_engine_current_state(&t->cc.eng) &
             (BR_SSL_SENDREC | BR_SSL_RECVREC |
              BR_SSL_SENDAPP | BR_SSL_RECVAPP)) == 0) {
            t->diag = 0x500;        /* engine idle mid-handshake */
            t->err = UAOS_TLS_E_IO;
            return t->err;
        }
    }
}

/* Decrypt application data into buf.  Returns bytes read, 0 on clean
 * close_notify / EOF, <0 on error. */
static int uaos_tls_recv(void *buf, int len)
{
    uaos_tls_t *t = &uaos_tls;

    for (;;) {
        unsigned st = br_ssl_engine_current_state(&t->cc.eng);

        if (st & BR_SSL_CLOSED)
            return br_ssl_engine_last_error(&t->cc.eng) ? -1 : 0;
        if (st & BR_SSL_RECVAPP) {
            size_t blen;
            unsigned char *b = br_ssl_engine_recvapp_buf(&t->cc.eng,
                                                         &blen);
            if ((int)blen > len)
                blen = (size_t)len;
            memcpy(buf, b, blen);
            br_ssl_engine_recvapp_ack(&t->cc.eng, blen);
            return (int)blen;
        }
        if (uaos_tls_pump(t, 1, 0) < 0)
            /* CLOSED with last_error==0 is a clean close_notify that
             * arrived inside the pump — report it as EOF. */
            return br_ssl_engine_last_error(&t->cc.eng) ? -1 : 0;
        if ((br_ssl_engine_current_state(&t->cc.eng) &
             (BR_SSL_RECVREC | BR_SSL_SENDREC)) == 0 &&
            (br_ssl_engine_current_state(&t->cc.eng) &
             BR_SSL_RECVAPP) == 0)
            return -1;   /* engine idle but wants nothing — deadlock guard */
    }
}

/* Encrypt and send application data.  Returns bytes accepted, <0 on
 * error. */
static int uaos_tls_send(const void *data, int len)
{
    uaos_tls_t *t = &uaos_tls;
    int sent = 0;

    while (sent < len) {
        unsigned st = br_ssl_engine_current_state(&t->cc.eng);

        if (st & BR_SSL_CLOSED)
            return -1;
        if (st & BR_SSL_SENDAPP) {
            size_t blen;
            unsigned char *b = br_ssl_engine_sendapp_buf(&t->cc.eng,
                                                         &blen);
            int want = len - sent;
            if (b == NULL || blen == 0) {
                if (uaos_tls_pump(t, 1, 1) < 0)
                    return -1;
                continue;
            }
            if ((int)blen > want)
                blen = (size_t)want;
            memcpy(b, (const char *)data + sent, blen);
            br_ssl_engine_sendapp_ack(&t->cc.eng, blen);
            /* sendapp_ack only emits the record when the buffer fills;
             * flush so short writes (e.g. an HTTP request) go out now. */
            br_ssl_engine_flush(&t->cc.eng, 0);
            sent += (int)blen;
            continue;
        }
        /* SENDAPP unavailable: service pending outbound records and
         * consume an inbound one if that is what the engine needs. */
        if (uaos_tls_pump(t, 1, 1) < 0)
            return -1;
        if ((br_ssl_engine_current_state(&t->cc.eng) &
             (BR_SSL_SENDREC | BR_SSL_RECVREC | BR_SSL_SENDAPP)) == 0)
            return -1;   /* nothing to do and no app channel — wedged */
    }
    /* Anything still buffered must reach the wire before we return;
     * the loop exits on sent==len while a SENDREC record may pend. */
    if (uaos_tls_pump(t, 0, 1) < 0)
        return -1;
    return sent;
}

/* Send close_notify if possible, then close the socket. */
static void uaos_tls_close(void)
{
    uaos_tls_t *t = &uaos_tls;

    if (t->fd < 0)
        return;
    if (t->ready &&
        (br_ssl_engine_current_state(&t->cc.eng) & BR_SSL_CLOSED) == 0) {
        br_ssl_engine_close(&t->cc.eng);
        uaos_tls_pump(t, 0, 1);   /* best-effort flush of close_notify */
    }
    uaos_sock_close(t->fd);
    t->fd = -1;
    t->ready = 0;
}

/* Short description for the last error (engine BR_ERR_* or ours). */
static const char *uaos_tls_errstr(int err)
{
    switch (err) {
    case UAOS_TLS_E_NOTIME: return "system clock unset (run ntpd) — cannot verify certificates";
    case UAOS_TLS_E_SEED:   return "kernel entropy unavailable";
    case UAOS_TLS_E_IO:     return "TLS transport failure";
    case UAOS_TLS_E_ARGS:   return "TLS init failure";
    }
    if (err >= BR_ERR_X509_INVALID_VALUE && err <= BR_ERR_X509_NOT_TRUSTED)
        return "certificate verification failed";
    switch (err) {
    case BR_ERR_BAD_CCS:        return "TLS: unexpected ChangeCipherSpec";
    case BR_ERR_BAD_ALERT:      return "TLS: alert from peer";
    case BR_ERR_BAD_HANDSHAKE:  return "TLS: handshake failed";
    case BR_ERR_NO_RANDOM:      return "TLS: RNG not seeded";
    case BR_ERR_LIMIT_EXCEEDED: return "TLS: record too large";
    }
    return "TLS error";
}

#endif /* UAOS_TLS_H */
