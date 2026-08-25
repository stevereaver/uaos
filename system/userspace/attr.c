/* attr.c — UAOS x86-64 userspace 'attr' command
 *
 * AmigaDOS C:Attr — show file attributes and protection bits.
 *   attr <path>
 */

#include "uaos_cmd.h"

static void prot_bit_str(char *buf, uint16_t prot)
{
    /* First four are "set" flags (hspa); last four are inverted
     * (bit set = denied, so '-' means allowed). */
    buf[0] = (prot & UAOS_FIBF_HOLD)    ? 'h' : '-';
    buf[1] = (prot & UAOS_FIBF_SCRIPT)  ? 's' : '-';
    buf[2] = (prot & UAOS_FIBF_PURE)    ? 'p' : '-';
    buf[3] = (prot & UAOS_FIBF_ARCHIVE) ? 'a' : '-';
    buf[4] = (prot & UAOS_FIBF_READ)    ? '-' : 'r';
    buf[5] = (prot & UAOS_FIBF_WRITE)   ? '-' : 'w';
    buf[6] = (prot & UAOS_FIBF_EXECUTE) ? '-' : 'e';
    buf[7] = (prot & UAOS_FIBF_DELETE)  ? '-' : 'd';
    buf[8] = '\0';
}

int main(int argc, const char **argv)
{
    if (argc < 2 || !argv[1][0]) {
        put_line("Usage: attr <path>");
        return 5;
    }

    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(argv[1], path, sizeof(path));

    long attrs = uaos_getattrs(path);
    /* If getattrs returned 0, the path may still exist as a file or dir. */
    struct uaos_stat st;
    if (attrs == 0 && uaos_stat(path, &st) != 0) {
        put_s("Not found: ");
        put_line(path);
        return 5;
    }
    if (attrs == 0 && st.is_dir) {
        /* directories may legitimately report 0 attrs */
    }

    long prot = uaos_getprotection(path);
    char pstr[16];
    prot_bit_str(pstr, (uint16_t)prot);

    char msg[UAOS_CMD_LINE_MAX];
    msg[0] = '\0';
    uaos_strlcat(msg, "Attributes: ", sizeof(msg));
    if (attrs & UAOS_ATTR_READONLY) uaos_strlcat(msg, "Read-Only ", sizeof(msg));
    if (attrs & UAOS_ATTR_HIDDEN)   uaos_strlcat(msg, "Hidden ",    sizeof(msg));
    if (attrs == 0)                 uaos_strlcat(msg, "Normal ",    sizeof(msg));
    uaos_strlcat(msg, " Protection: ", sizeof(msg));
    uaos_strlcat(msg, pstr, sizeof(msg));
    put_line(msg);
    return 0;
}
