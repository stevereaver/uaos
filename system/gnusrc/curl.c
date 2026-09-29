/* curl.c — curl-compatible transfer tool for UAOS gnu: layer
 *
 *   curl [OPTION]... URL
 *
 * Options: -o/--output FILE, -O/--remote-name, -s/--silent,
 *          -S/--show-error, -v/--verbose, -L/--location,
 *          -I/--head, -X/--request METHOD, -H/--header STR (repeatable),
 *          -d/--data STR, --data-binary @FILE, -A/--user-agent STR,
 *          -u/--user USER:PASS, -f/--fail, -k/--insecure,
 *          --connect-timeout SECS, --max-time SECS
 *
 * HTTP/1.1 via uaos_http.h over the kernel socket syscalls.  HTTPS URLs
 * fail cleanly until the TLS layer lands.
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"
#include "uaos_http.h"

#define CURL_MAX_HEADERS  24
#define CURL_MAX_DATA     (64 * 1024)

static int  opt_silent    = 0;
static int  opt_show_err  = 0;
static int  opt_verbose   = 0;
static int  opt_follow    = 0;
static int  opt_head      = 0;
static int  opt_remote_nm = 0;
static int  opt_fail      = 0;
static int  opt_insecure  = 0;    /* -k: skip certificate verification */
static int  opt_include   = 0;    /* -i: response head on output stream */
static const char *opt_output   = NULL;
static const char *opt_request  = NULL;
static const char *opt_agent    = NULL;
static const char *opt_user     = NULL;
static const char *opt_data     = NULL;   /* inline -d data */
static const char *opt_data_file = NULL;  /* --data-binary @file target */
static long opt_conn_to_s = 10;
static long opt_max_time_s = 0;           /* 0 = no overall limit */

static const char *opt_headers[CURL_MAX_HEADERS];
static int  opt_n_headers = 0;

/* curl-style exit codes we use */
#define CURL_E_USAGE     2
#define CURL_E_URL       3
#define CURL_E_DNS       6
#define CURL_E_CONNECT   7
#define CURL_E_HTTP      22
#define CURL_E_READFILE  26
#define CURL_E_TIMEOUT   28
#define CURL_E_WRITE     23
#define CURL_E_GENERIC   1

static void eprint(const char *s)
{
    /* stdout/stderr both route to the task console; keep messages off
     * the data path by suppressing them in --silent mode. */
    if (!opt_silent || opt_show_err)
        put_s(s);
}

static void vprint(const char *s)
{
    if (opt_verbose)
        put_s(s);
}

/* Map a uaos_http error to a curl exit code. */
static int herr_to_curl(int err)
{
    switch (err) {
    case UAOS_HERR_URL:     return CURL_E_URL;
    case UAOS_HERR_PROTO:   return CURL_E_URL;
    case UAOS_HERR_DNS:     return CURL_E_DNS;
    case UAOS_HERR_CONNECT: return CURL_E_CONNECT;
    case UAOS_HERR_SEND:
    case UAOS_HERR_RECV:
    case UAOS_HERR_CLOSED:  return CURL_E_TIMEOUT;
    }
    return CURL_E_GENERIC;
}

/* Load a -d @file / --data-binary @file body into memory. */
static uint8_t *load_body_file(const char *path, long *out_len)
{
    char abs[UAOS_CMD_PATH_MAX];
    cmd_make_abs(path, abs, sizeof(abs));
    long fd = uaos_open(abs, UAOS_O_RDONLY);
    if (fd < 0)
        return NULL;
    struct uaos_stat st;
    long size = (uaos_stat(abs, &st) == 0 && !st.is_dir) ? (long)st.size : 0;
    if (size <= 0 || size > CURL_MAX_DATA) {
        uaos_close((int)fd);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)uaos_alloc(size);
    if (!buf) {
        uaos_close((int)fd);
        return NULL;
    }
    long got = 0;
    while (got < size) {
        long n = uaos_read_file((int)fd, buf + got, size - got);
        if (n <= 0) break;
        got += n;
    }
    uaos_close((int)fd);
    if (got != size)
        return NULL;
    *out_len = size;
    return buf;
}

/* Perform one transfer (with redirect following when -L).  Returns a
 * curl-style exit code. */
static int transfer(const char *url_str)
{
    UaosUrl url;
    if (uaos_http_parse_url(url_str, &url) < 0) {
        eprint("curl: (3) malformed URL\n");
        return CURL_E_URL;
    }

    /* Prepare request body (if any) once — reused across redirects. */
    const uint8_t *body = NULL;
    long body_len = 0;
    uint8_t *file_body = NULL;
    if (opt_data_file) {
        file_body = load_body_file(opt_data_file, &body_len);
        if (!file_body) {
            eprint("curl: (26) could not read request data file\n");
            return CURL_E_READFILE;
        }
        body = file_body;
    } else if (opt_data) {
        body = (const uint8_t *)opt_data;
        body_len = (long)uaos_strlen(opt_data);
    }

    const char *method = opt_request;
    if (!method)
        method = opt_head ? "HEAD" : (body_len > 0 ? "POST" : "GET");

    /* Assemble extra headers: -H list, -u auth, -d content-type. */
    const char *req_hdrs[CURL_MAX_HEADERS + 2];
    int n_req = 0;
    for (int i = 0; i < opt_n_headers; i++)
        req_hdrs[n_req++] = opt_headers[i];
    char auth_hdr[192];
    if (opt_user) {
        uaos_http_basic_auth(opt_user, auth_hdr, sizeof(auth_hdr));
        req_hdrs[n_req++] = auth_hdr;
    }
    if (body_len > 0)
        req_hdrs[n_req++] = "Content-Type: application/x-www-form-urlencoded";

    uint32_t timeout_ms = opt_max_time_s > 0 ? (uint32_t)opt_max_time_s * 1000
                                             : (uint32_t)opt_conn_to_s * 1000;

    int redirects = 0;
    for (;;) {
        UaosHttp h;
        int rc = uaos_http_open(&h, &url, method, req_hdrs, n_req,
                                body, body_len,
                                opt_agent, timeout_ms, opt_insecure);
        if (rc < 0) {
            char nb[12];
            uaos_http_dec(rc, nb);
            eprint("curl: ("); eprint(nb); eprint(") ");
            if (rc == UAOS_HERR_TLS)
                eprint(uaos_tls_errstr(h.tls_err));
            else
                eprint(uaos_http_errstr(rc));
            eprint("\n");
            return herr_to_curl(rc);
        }

        if (opt_verbose) {
            vprint("< "); vprint(h.status_line); vprint("\n");
            for (int i = 0; i < h.hdr_count; i++) {
                vprint("< "); vprint(h.hdrs[i].name);
                vprint(": "); vprint(h.hdrs[i].value); vprint("\n");
            }
            vprint("< \n");
        }

        if (opt_follow && uaos_http_is_redirect(h.status) &&
            uaos_http_header(&h, "Location")) {
            if (redirects >= UAOS_HTTP_MAX_REDIRECT) {
                uaos_http_close(&h);
                eprint("curl: (47) redirect limit exceeded\n");
                return CURL_E_GENERIC;
            }
            UaosUrl next;
            int rr = uaos_http_resolve_redirect(&url,
                                                uaos_http_header(&h, "Location"),
                                                &next);
            uaos_http_close(&h);
            if (rr < 0) {
                eprint("curl: (3) bad redirect Location\n");
                return CURL_E_URL;
            }
            url = next;
            /* 303 → GET with no body; 301/302 likewise for POST (curl's
             * default behaviour for non-307/308). */
            if (h.status == 303 || ((h.status == 301 || h.status == 302) &&
                                    body_len > 0)) {
                method = "GET";
                body = NULL;
                body_len = 0;
                /* drop the Content-Type we added for the POST body */
                n_req--;
            }
            redirects++;
            continue;
        }

        if (opt_fail && h.status >= 400) {
            char nb[12];
            uaos_http_dec(h.status, nb);
            eprint("curl: (22) The requested URL returned error: ");
            eprint(nb); eprint("\n");
            uaos_http_close(&h);
            return CURL_E_HTTP;
        }

        /* Output target: stdout unless -o/-O. */
        int to_stdout = 1;
        long ofd = -1;
        char fname[UAOS_CMD_PATH_MAX];
        if (opt_output) {
            uaos_strncpy(fname, opt_output, sizeof(fname));
            to_stdout = 0;
        } else if (opt_remote_nm) {
            uaos_strncpy(fname, uaos_http_url_filename(&url), sizeof(fname));
            to_stdout = 0;
        }
        if (!to_stdout) {
            char abs[UAOS_CMD_PATH_MAX];
            cmd_make_abs(fname, abs, sizeof(abs));
            ofd = uaos_open(abs, UAOS_O_WRONLY | UAOS_O_CREAT | UAOS_O_TRUNC);
            if (ofd < 0) {
                eprint("curl: (23) cannot write output file\n");
                uaos_http_close(&h);
                return CURL_E_WRITE;
            }
            if (opt_verbose) {
                vprint("* Saving to '"); vprint(fname); vprint("'\n");
            }
        }

        /* -i: emit the response head on the output stream before body. */
        if (opt_include) {
            char line[512];
            int lw = 0;
            const char *sl = h.status_line;
            while (*sl && lw < (int)sizeof(line) - 3) line[lw++] = *sl++;
            line[lw++] = '\r'; line[lw++] = '\n'; line[lw] = '\0';
            if (to_stdout) uaos_write(1, line, lw);
            else           uaos_write_file((int)ofd, line, lw);
            for (int i = 0; i < h.hdr_count; i++) {
                lw = 0;
                const char *n = h.hdrs[i].name, *v = h.hdrs[i].value;
                while (*n && lw < (int)sizeof(line) - 4) line[lw++] = *n++;
                if (lw < (int)sizeof(line) - 3) { line[lw++] = ':'; line[lw++] = ' '; }
                while (*v && lw < (int)sizeof(line) - 3) line[lw++] = *v++;
                line[lw++] = '\r'; line[lw++] = '\n';
                if (to_stdout) uaos_write(1, line, lw);
                else           uaos_write_file((int)ofd, line, lw);
            }
            if (to_stdout) uaos_write(1, "\r\n", 2);
            else           uaos_write_file((int)ofd, "\r\n", 2);
        }

        uint8_t buf[4096];
        long total = 0;
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
        }
        uaos_http_close(&h);
        if (ofd >= 0)
            uaos_close((int)ofd);
        if (io_err) {
            eprint("curl: (18) transfer closed with outstanding read data\n");
            return CURL_E_TIMEOUT;
        }
        if (opt_verbose) {
            char nb[16];
            uaos_http_dec(total, nb);
            vprint("* "); vprint(nb); vprint(" bytes received\n");
        }
        return 0;
    }
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"output",          'o', required_argument},
        {"remote-name",     'O', no_argument},
        {"silent",          's', no_argument},
        {"show-error",      'S', no_argument},
        {"verbose",         'v', no_argument},
        {"location",        'L', no_argument},
        {"head",            'I', no_argument},
        {"request",         'X', required_argument},
        {"header",          'H', required_argument},
        {"data",            'd', required_argument},
        {"data-binary",     0,   required_argument},   /* +10 */
        {"user-agent",      'A', required_argument},
        {"user",            'u', required_argument},
        {"fail",            'f', no_argument},
        {"insecure",        'k', no_argument},
        {"connect-timeout", 0,   required_argument},   /* +15 */
        {"max-time",        0,   required_argument},   /* +16 */
        {"include",         'i', no_argument},         /* response headers to stdout */
        {"help",            0,   no_argument},         /* +18 */
        {"version",         0,   no_argument},         /* +19 */
        {NULL, 0, 0}
    };
#define LONG_DATABIN   (UAOS_GO_LONG + 10)
#define LONG_CONNTIME  (UAOS_GO_LONG + 15)
#define LONG_MAXTIME   (UAOS_GO_LONG + 16)
#define LONG_HELP      (UAOS_GO_LONG + 18)
#define LONG_VERSION   (UAOS_GO_LONG + 19)
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv,
                                   "o:OsSvLIX:H:d:A:u:fik",
                                   long_opts, &li)) != -1) {
        switch (opt) {
        case 'o': opt_output = g_optarg; break;
        case 'O': opt_remote_nm = 1; break;
        case 's': opt_silent = 1; break;
        case 'S': opt_show_err = 1; break;
        case 'v': opt_verbose = 1; break;
        case 'L': opt_follow = 1; break;
        case 'I': opt_head = 1; opt_include = 1; break;
        case 'X': opt_request = g_optarg; break;
        case 'H':
            if (g_optarg && opt_n_headers < CURL_MAX_HEADERS)
                opt_headers[opt_n_headers++] = g_optarg;
            break;
        case 'd': opt_data = g_optarg; break;
        case 'A': opt_agent = g_optarg; break;
        case 'u': opt_user = g_optarg; break;
        case 'f': opt_fail = 1; break;
        case 'i': opt_include = 1; break;
        case LONG_DATABIN:
            if (g_optarg && g_optarg[0] == '@')
                opt_data_file = g_optarg + 1;
            else if (g_optarg)
                opt_data = g_optarg;
            break;
        case LONG_CONNTIME:
            { long v; if (uaos_optarg_long(&v) && v > 0) opt_conn_to_s = v; }
            break;
        case LONG_MAXTIME:
            { long v; if (uaos_optarg_long(&v) && v > 0) opt_max_time_s = v; }
            break;
        case 'k': opt_insecure = 1; break;
        case LONG_HELP:
            put_line("Usage: curl [OPTION]... URL");
            put_line("  -o, --output FILE        write to FILE (default: stdout)");
            put_line("  -O, --remote-name        write to URL basename");
            put_line("  -s, --silent             silent mode");
            put_line("  -S, --show-error         show errors in silent mode");
            put_line("  -v, --verbose            request/response trace");
            put_line("  -L, --location           follow redirects");
            put_line("  -I, --head               HEAD request");
            put_line("  -X, --request METHOD     custom method");
            put_line("  -H, --header STR         extra request header");
            put_line("  -d, --data STR           POST body data");
            put_line("      --data-binary @FILE  POST body from file");
            put_line("  -A, --user-agent STR     User-Agent header");
            put_line("  -u, --user USER:PASS     Basic auth");
            put_line("  -f, --fail               fail on HTTP errors");
            put_line("  -k, --insecure           skip TLS certificate verification");
            put_line("      --connect-timeout S  connect timeout (seconds)");
            put_line("      --max-time S         overall timeout (seconds)");
            return 0;
        case LONG_VERSION:
            put_line("UAOS curl 1.0 (compatible subset)");
            return 0;
        case '?':
            eprint("curl: unknown or invalid option — try --help\n");
            return CURL_E_USAGE;
        default:
            return CURL_E_USAGE;
        }
    }

    if (uaos_operands_count(argc) < 1) {
        eprint("curl: (2) no URL specified — try --help\n");
        return CURL_E_USAGE;
    }

    int rc = 0;
    for (int i = 0; i < uaos_operands_count(argc); i++) {
        const char *url = uaos_operand(argc, argv, i);
        if (!url) continue;
        int r = transfer(url);
        if (r != 0) rc = r;
    }
    return rc;
}
