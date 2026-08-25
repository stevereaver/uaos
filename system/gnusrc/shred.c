/* shred.c — GNU coreutils 'shred' for UAOS gnu: layer
 *
 * Overwrite a file to hide its contents, and optionally delete it.
 *   shred [OPTION]... FILE...
 * Options: -n N, --iterations=N, -u, --remove, -z, --zero, -v, --verbose, -f, --force
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_iterations = 3;
static int opt_remove = 0;
static int opt_zero = 0;
static int opt_verbose = 0;
static int opt_force = 0;

static uint32_t shred_state = 0xCAFEBABE;
static uint8_t shred_rand(void)
{
    uint32_t x = shred_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    shred_state = x;
    return (uint8_t)(x & 0xFF);
}

static int do_shred(const char *fname)
{
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(fname, path, sizeof(path));
    struct uaos_stat st;
    if (uaos_stat(path, &st) != 0) {
        if (opt_force) {
            /* create the file */
            long fd = uaos_open(path, UAOS_O_WRONLY | UAOS_O_CREAT);
            if (fd < 0) { put_s("shred: cannot open '"); put_s(fname); put_line("'"); return 1; }
            uaos_close((int)fd);
            uaos_stat(path, &st);
        } else {
            put_s("shred: cannot open '"); put_s(fname); put_line("'"); return 1;
        }
    }

    for (int pass = 0; pass < opt_iterations; pass++) {
        long fd = uaos_open(path, UAOS_O_WRONLY);
        if (fd < 0) { put_s("shred: cannot open '"); put_s(fname); put_line("'"); return 1; }
        uint32_t pos = 0;
        while (pos < st.size) {
            uint8_t buf[4096];
            long chunk = (long)(st.size - pos < sizeof(buf) ? st.size - pos : sizeof(buf));
            for (long i = 0; i < chunk; i++) buf[i] = shred_rand();
            uaos_write_file((int)fd, buf, chunk);
            pos += (uint32_t)chunk;
        }
        uaos_close((int)fd);
        if (opt_verbose) {
            put_s("shred: "); put_s(fname); put_s(": pass ");
            char buf[8]; int_to_dec(pass + 1, buf, sizeof(buf)); put_s(buf);
            put_c('/'); int_to_dec(opt_iterations, buf, sizeof(buf)); put_s(buf);
            put_c('\n');
        }
    }

    if (opt_zero) {
        long fd = uaos_open(path, UAOS_O_WRONLY);
        if (fd >= 0) {
            uint32_t pos = 0;
            while (pos < st.size) {
                uint8_t buf[4096];
                long chunk = (long)(st.size - pos < sizeof(buf) ? st.size - pos : sizeof(buf));
                uaos_memset(buf, 0, chunk);
                uaos_write_file((int)fd, buf, chunk);
                pos += (uint32_t)chunk;
            }
            uaos_close((int)fd);
        }
    }

    if (opt_remove) {
        uaos_delete(path);
        if (opt_verbose) { put_s("shred: "); put_s(fname); put_line(": removed"); }
    }
    return 0;
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"iterations", 'n', required_argument},
        {"remove",     'u', no_argument},
        {"zero",       'z', no_argument},
        {"verbose",    'v', no_argument},
        {"force",      'f', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "n:uzvf", long_opts, &li)) != -1) {
        switch (opt) {
            case 'n': { long v; if (uaos_optarg_long(&v)) opt_iterations = (int)v; } break;
            case 'u': opt_remove = 1; break;
            case 'z': opt_zero = 1; break;
            case 'v': opt_verbose = 1; break;
            case 'f': opt_force = 1; break;
            default:  return 1;
        }
    }
    int nops = uaos_operands_count(argc);
    if (nops == 0) { put_line("shred: missing file operand"); return 1; }
    int rc = 0;
    for (int i = 0; i < nops; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (fname) { if (do_shred(fname) != 0) rc = 1; }
    }
    return rc;
}
