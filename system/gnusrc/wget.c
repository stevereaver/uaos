/* wget.c — GNU wget-compatible downloader for UAOS gnu: layer
 *
 *   wget [OPTION]... URL
 *
 * Options: -O/--output-document FILE ('-' = stdout), -q/--quiet,
 *          -nv/--no-verbose, -c/--continue, -t/--tries N,
 *          --timeout SECS, --max-redirect N, --user-agent STR,
 *          --header STR (repeatable), --no-check-certificate
 *
 * HTTP/1.1 via uaos_http.h over the kernel socket syscalls.  HTTPS URLs
 * fail cleanly until the TLS layer lands.
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"
#include "uaos_http.h"

#define WGET_MAX_HEADERS 16

static int  opt_quiet      = 0;
static int  opt_noverbose  = 0;
static int  opt_resume     = 0;
static long opt_tries      = 1;
static long opt_timeout_s  = 30;
static long opt_max_redir  = UAOS_HTTP_MAX_REDIRECT;
static const char *opt_output    = NULL;
static const char *opt_agent     = "UAOS-wget/1.0";
static const char *opt_headers[WGET_MAX_HEADERS];
static int  opt_n_headers  = 0;
static int  opt_no_check_cert = 0;   /* --no-check-certificate */
static int  seen_dash_n    = 0;      /* "-nv" arrives as combined 'n','v' */

static void vprint(const char *s)
{
    if (!opt_quiet && !opt_noverbose)
        put_s(s);
}

static void vline(const char *s)
{
    if (!opt_quiet && !opt_noverbose)
        put_line(s);
}

static void fmt_size(long n, char *out, int max)
{
    char num[24];
    uaos_http_dec(n, num);
    int w = 0;
    if (n >= 1024 * 1024) {
        uaos_http_dec(n / (1024 * 1024), num);
        while (num[w] && w < max - 3) { out[w] = num[w]; w++; }
        out[w++] = 'M';
    } else if (n >= 1024) {
        uaos_http_dec(n / 1024, num);
        while (num[w] && w < max - 3) { out[w] = num[w]; w++; }
        out[w++] = 'K';
    } else {
        while (num[w] && w < max - 2) { out[w] = num[w]; w++; }
    }
    out[w] = '\0';
}

/* Choose the output filename for a URL: -O argument, or the basename of
 * the URL path ("index.html" for a bare directory URL). */
static void pick_filename(const UaosUrl *u, char *out, int max)
{
    if (opt_output) {
        uaos_strncpy(out, opt_output, (size_t)max);
        return;
    }
    uaos_strncpy(out, uaos_http_url_filename(u), (size_t)max);
}

/* Re-render a UaosUrl as "scheme://host[:port]/path" for display. */
static void fmt_url(const UaosUrl *u, char *out, int max)
{
    int w = 0;
    const char *s = u->scheme;
    while (*s && w < max - 8) out[w++] = *s++;
    out[w++] = ':'; out[w++] = '/'; out[w++] = '/';
    for (s = u->host; *s && w < max - 8; ) out[w++] = *s++;
    int dflt = (u->port == 80 && !uaos_strcmp(u->scheme, "http")) ||
               (u->port == 443 && !uaos_strcmp(u->scheme, "https"));
    if (!dflt && w < max - 8) {
        char pb[8];
        uaos_http_dec(u->port, pb);
        out[w++] = ':';
        for (s = pb; *s && w < max - 2; ) out[w++] = *s++;
    }
    for (s = u->path; *s && w < max - 1; ) out[w++] = *s++;
    out[w] = '\0';
}

/* One fetch attempt against url; follows redirects internally.
 * Returns 0 on success, UAOS_HERR_* (<0) on network error, or the HTTP
 * status code (>0) for a non-2xx response. */
static int fetch(const char *url_str)
{
    UaosUrl url;
    int rc = uaos_http_parse_url(url_str, &url);
    if (rc < 0) {
        if (!opt_quiet) {
            put_s("wget: "); put_s(url_str);
            put_s(": "); put_line(uaos_http_errstr(rc));
        }
        return rc;
    }

    int redirects = 0;
    for (;;) {
        char fname[UAOS_CMD_PATH_MAX];
        pick_filename(&url, fname, sizeof(fname));

        /* -c resume: offset = size of the existing output file. */
        long resume_off = 0;
        char range_hdr[48];
        const char *req_hdrs[WGET_MAX_HEADERS + 2];
        int n_req = 0;
        for (int i = 0; i < opt_n_headers; i++)
            req_hdrs[n_req++] = opt_headers[i];
        int stdout_mode = opt_output && !uaos_strcmp(opt_output, "-");
        if (opt_resume && !stdout_mode) {
            char abs[UAOS_CMD_PATH_MAX];
            cmd_make_abs(fname, abs, sizeof(abs));
            struct uaos_stat st;
            if (uaos_stat(abs, &st) == 0 && !st.is_dir && st.size > 0) {
                resume_off = st.size;
                uaos_strcpy(range_hdr, "Range: bytes=");
                uaos_http_dec(resume_off, range_hdr + 13);
                uaos_strlcat(range_hdr, "-", sizeof(range_hdr));
                req_hdrs[n_req++] = range_hdr;
            }
        }


        {
            char us[UAOS_HTTP_PATH_MAX + UAOS_HTTP_HOST_MAX + 16];
            fmt_url(&url, us, sizeof(us));
            vprint("--  "); vline(us);
        }
        vprint("Resolving "); vprint(url.host); vline("...");

        UaosHttp h;
        rc = uaos_http_open(&h, &url, "GET", req_hdrs, n_req,
                            NULL, 0, opt_agent,
                            (uint32_t)opt_timeout_s * 1000,
                            opt_no_check_cert);
        if (rc < 0) {
            if (!opt_quiet) {
                put_s("wget: "); put_s(url.host); put_s(": ");
                if (h.err == UAOS_HERR_TLS) {
                    put_s(uaos_tls_errstr(h.tls_err));
                    {
                        char nb[24];
                        put_s(" [err=");
                        uaos_http_dec(h.tls_err, nb); put_s(nb);
                        put_s(" diag=");
                        uaos_http_dec((long)uaos_tls.diag, nb); put_s(nb);
                        put_s(" xerr=");
                        uaos_http_dec(uaos_tls.xerr, nb); put_s(nb);
                        put_s("]");
                    }
                    put_s("\n");
                } else {
                    put_line(uaos_http_errstr(h.err ? h.err : rc));
                }
            }
            return rc;
        }

        if (uaos_http_is_redirect(h.status) && uaos_http_header(&h, "Location")) {
            if (redirects >= opt_max_redir) {
                uaos_http_close(&h);
                if (!opt_quiet) put_line("wget: redirect limit exceeded");
                return UAOS_HERR_URL;
            }
            const char *loc = uaos_http_header(&h, "Location");
            vprint("HTTP request sent, awaiting response... ");
            vline(h.status_line[0] ? h.status_line + 9 : "redirect");
            vprint("Location: "); vprint(loc); vline(" [following]");
            UaosUrl next;
            int rr = uaos_http_resolve_redirect(&url, loc, &next);
            uaos_http_close(&h);
            if (rr < 0)
                return rr;
            url = next;
            redirects++;
            continue;
        }

        if (h.status == 416 && resume_off > 0) {
            /* Range unsatisfiable because the local file is already
             * complete — wget treats this as nothing left to do. */
            uaos_http_close(&h);
            vline("The file is already fully downloaded; nothing to do.");
            return 0;
        }

        if (h.status != 200 && h.status != 206) {
            char nb[12];
            uaos_http_dec(h.status, nb);
            vprint("HTTP request sent, awaiting response... ");
            vline(h.status_line[0] ? h.status_line + 9 : nb);
            uaos_http_close(&h);
            return h.status;
        }

        if (resume_off > 0 && h.status == 200) {
            /* Server ignored Range — restart from scratch. */
            resume_off = 0;
        }

        vprint("HTTP request sent, awaiting response... ");
        vline(h.status_line[0] ? h.status_line + 9 : "200");
        if (h.content_length >= 0) {
            char nb[24], sb[24];
            uaos_http_dec(h.content_length, nb);
            fmt_size(h.content_length, sb, sizeof(sb));
            vprint("Length: "); vprint(nb);
            vprint(" ("); vprint(sb); vline(")");
        }

        /* Open the output target. */
        int to_stdout = opt_output && !uaos_strcmp(opt_output, "-");
        long ofd = -1;
        if (!to_stdout) {
            char abs[UAOS_CMD_PATH_MAX];
            cmd_make_abs(fname, abs, sizeof(abs));
            if (resume_off > 0) {
                ofd = uaos_open(abs, UAOS_O_WRONLY);
                if (ofd >= 0)
                    uaos_seek((int)ofd, (uint32_t)resume_off);
            } else {
                ofd = uaos_open(abs, UAOS_O_WRONLY | UAOS_O_CREAT | UAOS_O_TRUNC);
            }
            if (ofd < 0) {
                put_s("wget: cannot write to '"); put_s(fname); put_line("'");
                uaos_http_close(&h);
                return UAOS_HERR_URL;
            }
            vprint("Saving to: '"); vprint(fname); vline("'");
        } else {
            vline("(output to stdout)");
        }

        /* Stream the body. */
        uint8_t buf[4096];
        char sb[24];
        long total = resume_off;
        long next_report = 65536;
        int io_err = 0;
        for (;;) {
            long n = uaos_http_read(&h, buf, sizeof(buf));
            if (n < 0) { io_err = (int)n; break; }
            if (n == 0) break;
            if (to_stdout) {
                long off = 0;
                while (off < n) {
                    long w = uaos_write(1, buf + off, n - off);
                    if (w <= 0) { io_err = UAOS_HERR_CLOSED; break; }
                    off += w;
                }
            } else {
                long off = 0;
                while (off < n) {
                    long w = uaos_write_file((int)ofd, buf + off, n - off);
                    if (w <= 0) { io_err = UAOS_HERR_CLOSED; break; }
                    off += w;
                }
            }
            if (io_err) break;
            total += n;
            if (!opt_quiet && !opt_noverbose && total >= next_report) {
                fmt_size(total, sb, sizeof(sb));
                vprint("  "); vprint(sb); vline(" ...");
                next_report += 65536;
            }
        }
        uaos_http_close(&h);
        if (ofd >= 0)
            uaos_close((int)ofd);
        if (io_err)
            return io_err;

        /* Verify byte count when the server gave us a length. */
        if (h.content_length >= 0 && h.body_total != h.content_length) {
            if (!opt_quiet)
                put_line("wget: transfer truncated (Content-Length mismatch)");
            return UAOS_HERR_RECV;
        }

        fmt_size(total, sb, sizeof(sb));
        vprint("Saved "); vprint(sb); vprint(" [");
        vprint(fname); vline("]");
        return 0;
    }
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"output-document",      'O', required_argument},
        {"quiet",                'q', no_argument},
        {"verbose",              'v', no_argument},    /* default; accepted */
        {"no-verbose",           0,   no_argument},    /* UAOS_GO_LONG+3 */
        {"continue",             'c', no_argument},
        {"tries",                't', required_argument},
        {"timeout",              'T', required_argument},
        {"max-redirect",         0,   required_argument}, /* +7 */
        {"user-agent",           'U', required_argument},
        {"header",               0,   required_argument}, /* +9 */
        {"no-check-certificate", 0,   no_argument},     /* +10 */
        {"help",                 0,   no_argument},     /* +11 */
        {"version",              0,   no_argument},     /* +12 */
        {NULL, 0, 0}
    };
#define LONG_NOVERBOSE  (UAOS_GO_LONG + 3)
#define LONG_MAXREDIR   (UAOS_GO_LONG + 7)
#define LONG_HEADER     (UAOS_GO_LONG + 9)
#define LONG_NOCHECKCRT (UAOS_GO_LONG + 10)
#define LONG_HELP       (UAOS_GO_LONG + 11)
#define LONG_VERSION    (UAOS_GO_LONG + 12)

    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "O:qnvct:T:U:",
                                   long_opts, &li)) != -1) {
        switch (opt) {
        case 'O': opt_output = g_optarg; break;
        case 'q': opt_quiet = 1; break;
        case 'n': seen_dash_n = 1; break;   /* "-nv" → 'n' then 'v' */
        case 'v': if (seen_dash_n) opt_noverbose = 1; break;
        case 'c': opt_resume = 1; break;
        case 't':
            if (!uaos_optarg_long(&opt_tries) || opt_tries < 1) opt_tries = 1;
            break;
        case 'T':
            if (!uaos_optarg_long(&opt_timeout_s) || opt_timeout_s < 1)
                opt_timeout_s = 30;
            break;
        case 'U': opt_agent = g_optarg; break;
        case LONG_NOVERBOSE: opt_noverbose = 1; break;
        case LONG_MAXREDIR:
            { long v; if (uaos_optarg_long(&v) && v >= 0) opt_max_redir = v; }
            break;
        case LONG_HEADER:
            if (g_optarg && opt_n_headers < WGET_MAX_HEADERS)
                opt_headers[opt_n_headers++] = g_optarg;
            break;
        case LONG_NOCHECKCRT: opt_no_check_cert = 1; break;
        case LONG_HELP:
            put_line("Usage: wget [OPTION]... URL");
            put_line("  -O, --output-document FILE   write to FILE ('-' = stdout)");
            put_line("  -q, --quiet                  quiet mode");
            put_line("      --no-verbose             less chatter (-nv)");
            put_line("  -c, --continue               resume a partial download");
            put_line("  -t, --tries N                retry N times");
            put_line("      --timeout SECS           network timeout");
            put_line("      --max-redirect N         redirect limit");
            put_line("  -U, --user-agent STR         User-Agent header");
            put_line("      --header STR             extra request header");
            put_line("      --no-check-certificate   skip TLS certificate verification");
            return 0;
        case LONG_VERSION:
            put_line("UAOS wget 1.0 (compatible subset)");
            return 0;
        case '?':
            put_line("wget: unknown or invalid option — try --help");
            return 1;
        default:
            return 1;
        }
    }

    if (uaos_operands_count(argc) < 1) {
        put_line("wget: missing URL — try --help");
        return 1;
    }

    int rc = 0;
    for (int i = 0; i < uaos_operands_count(argc); i++) {
        const char *url = uaos_operand(argc, argv, i);
        if (!url) continue;
        int ok = 0;
        for (int t = 0; t < opt_tries && !ok; t++) {
            int r = fetch(url);
            if (r == 0) { ok = 1; break; }
            rc = r;
            /* Don't retry 4xx client errors. */
            if (r >= 400 && r < 500) break;
            if (t + 1 < opt_tries) {
                vline("Retrying...");
                uaos_sleep_ms(500);
            }
        }
        if (!ok) {
            /* Map to wget-style exit codes: 4 network, 8 server response. */
            if (rc >= 400) rc = 8;
            else rc = 4;
        }
    }
    return rc;
}
