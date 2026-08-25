/* chmod.c — GNU coreutils 'chmod' for UAOS gnu: layer
 *
 * Change access permissions of files.
 *   chmod [OPTION]... MODE[,MODE]... FILE...
 *   chmod [OPTION]... OCTAL-MODE FILE...
 * Options: -R, --recursive, -v, --verbose, -c, --changes, -f, --silent
 *
 * Note: UAOS uses AmigaDOS protection bits, not POSIX permission bits.
 * This tool maps POSIX octal modes to AmigaDOS protection bits.
 *
 * POSIX rwxrwxrwx (octal 0-777) maps to AmigaDOS FIBF bits:
 *   owner read  -> FIBF_READ  (bit 3, inverted: set=disallowed)
 *   owner write -> FIBF_WRITE (bit 4, inverted)
 *   owner exec  -> FIBF_EXECUTE (bit 5, inverted)
 *   Group/other bits are merged with owner bits (single-user OS).
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_recursive = 0;
static int opt_verbose = 0;

static uint16_t parse_mode(const char *mode_str, uint16_t old_prot)
{
    /* octal mode: 0NNN */
    if (mode_str[0] >= '0' && mode_str[0] <= '7') {
        long v = 0;
        const char *p = mode_str;
        while (*p >= '0' && *p <= '7') { v = v * 8 + (*p - '0'); p++; }
        /* extract rwx for user (bits 8-6 of octal) */
        int r = (v >> 2) & 1; /* read */
        int w = (v >> 1) & 1; /* write */
        int x = v & 1;        /* execute */
        /* map to AmigaDOS inverted bits */
        uint16_t prot = 0;
        if (!r) prot |= UAOS_FIBF_READ;
        if (!w) prot |= UAOS_FIBF_WRITE;
        if (!x) prot |= UAOS_FIBF_EXECUTE;
        /* preserve delete bit from old protection */
        prot |= (old_prot & UAOS_FIBF_DELETE);
        return prot;
    }

    /* symbolic mode: [ugoa]*[+-=][rwx]* */
    /* simplified: just handle a+/-/=rwx patterns */
    int who = 0; /* 0 = all */
    const char *p = mode_str;
    while (*p && *p != '+' && *p != '-' && *p != '=') {
        if (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') who = 1;
        p++;
    }
    if (!*p) return old_prot;
    char op = *p++;
    int set_r = 0, set_w = 0, set_x = 0;
    while (*p) {
        if (*p == 'r') set_r = 1;
        else if (*p == 'w') set_w = 1;
        else if (*p == 'x') set_x = 1;
        p++;
    }

    uint16_t prot = old_prot;
    if (op == '=') {
        prot &= ~(UAOS_FIBF_READ | UAOS_FIBF_WRITE | UAOS_FIBF_EXECUTE);
        if (!set_r) prot |= UAOS_FIBF_READ;
        if (!set_w) prot |= UAOS_FIBF_WRITE;
        if (!set_x) prot |= UAOS_FIBF_EXECUTE;
    } else if (op == '+') {
        if (set_r) prot &= ~UAOS_FIBF_READ;
        if (set_w) prot &= ~UAOS_FIBF_WRITE;
        if (set_x) prot &= ~UAOS_FIBF_EXECUTE;
    } else if (op == '-') {
        if (set_r) prot |= UAOS_FIBF_READ;
        if (set_w) prot |= UAOS_FIBF_WRITE;
        if (set_x) prot |= UAOS_FIBF_EXECUTE;
    }
    return prot;
}

static int do_chmod(const char *fname, uint16_t mode)
{
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(fname, path, sizeof(path));
    struct uaos_stat st;
    if (uaos_stat(path, &st) != 0) {
        put_s("chmod: cannot access '"); put_s(fname); put_line("'");
        return 1;
    }

    if (st.is_dir && opt_recursive) {
        long dd = uaos_opendir(path);
        if (dd >= 0) {
            struct uaos_dirent ent;
            while (uaos_readdir((int)dd, &ent) > 0) {
                if (ent.name[0] == '.' && (ent.name[1] == '\0' ||
                    (ent.name[1] == '.' && ent.name[2] == '\0'))) continue;
                char child[UAOS_CMD_PATH_MAX];
                cmd_join_path(path, ent.name, child, sizeof(child));
                do_chmod(child, mode);
            }
            uaos_closedir((int)dd);
        }
    }

    if (uaos_setprotection(path, mode) != 0) {
        put_s("chmod: cannot change permissions of '"); put_s(fname); put_line("'");
        return 1;
    }
    if (opt_verbose) {
        put_s("mode of '"); put_s(fname); put_s("' changed to 0");
        char buf[8]; uint_to_dec(mode, buf, sizeof(buf)); put_s(buf); put_c('\n');
    }
    return 0;
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"recursive", 'R', no_argument},
        {"verbose",   'v', no_argument},
        {"changes",   'c', no_argument},
        {"silent",    'f', no_argument},
        {"reference", 0,  required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "Rvcf", long_opts, &li)) != -1) {
        switch (opt) {
            case 'R': opt_recursive = 1; break;
            case 'v': opt_verbose = 1; break;
            case 'c': opt_verbose = 1; break;
            case 'f': break;
            case UAOS_GO_LONG + 0: break; /* reference — not fully supported */
            default:  return 1;
        }
    }

    int nops = uaos_operands_count(argc);
    if (nops < 2) { put_line("chmod: missing operand"); return 1; }

    const char *mode_str = uaos_operand(argc, argv, 0);
    /* get old protection from first file to compute relative modes */
    char first_path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(uaos_operand(argc, argv, 1), first_path, sizeof(first_path));
    struct uaos_stat st;
    uint16_t old_prot = 0;
    if (uaos_stat(first_path, &st) == 0) old_prot = st.protection;
    uint16_t mode = parse_mode(mode_str, old_prot);

    int rc = 0;
    for (int i = 1; i < nops; i++) {
        const char *fname = uaos_operand(argc, argv, i);
        if (fname) { if (do_chmod(fname, mode) != 0) rc = 1; }
    }
    return rc;
}
