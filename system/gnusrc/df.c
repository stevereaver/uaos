/* df.c — GNU coreutils 'df' for UAOS gnu: layer
 *
 * Report file system disk space usage.
 *   df [OPTION]... [FILE]...
 * Options: -h, --human-readable, -k, --kibibytes, -i, --inodes
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_human = 0;
static int opt_kibi = 0;
static int opt_inodes = 0;

static void format_h(uint32_t sz, char *buf, int max)
{
    if (opt_human) {
        if (sz >= 1073741824U) { uint_to_dec(sz / 1073741824U, buf, max); uaos_strlcat(buf, "G", max); }
        else if (sz >= 1048576U) { uint_to_dec(sz / 1048576U, buf, max); uaos_strlcat(buf, "M", max); }
        else if (sz >= 1024) { uint_to_dec(sz / 1024, buf, max); uaos_strlcat(buf, "K", max); }
        else { uint_to_dec(sz, buf, max); }
    } else {
        /* default 1K blocks */
        uint_to_dec((sz + 1023) / 1024, buf, max);
    }
}

static void print_volume(const char *name, const char *mount)
{
    uint32_t total = 0, used = 0;
    if (uaos_getvolumeinfo(mount, &total, &used) != 0) return;
    uint32_t avail = total - used;
    uint32_t pct = total > 0 ? (used * 100 / total) : 0;

    char tbuf[16], ubuf[16], abuf[16];
    format_h(total, tbuf, sizeof(tbuf));
    format_h(used, ubuf, sizeof(ubuf));
    format_h(avail, abuf, sizeof(abuf));

    /* pad name to 16, pad numbers to 10 right-justified */
    put_s(name);
    int nl = (int)uaos_strlen(name);
    for (int i = nl; i < 16; i++) put_c(' ');

    int tl = (int)uaos_strlen(tbuf);
    for (int i = tl; i < 10; i++) put_c(' ');
    put_s(tbuf);
    int ul = (int)uaos_strlen(ubuf);
    for (int i = ul; i < 10; i++) put_c(' ');
    put_s(ubuf);
    int al = (int)uaos_strlen(abuf);
    for (int i = al; i < 10; i++) put_c(' ');
    put_s(abuf);

    put_s("  ");
    char pbuf[8]; uint_to_dec(pct, pbuf, sizeof(pbuf));
    int pl = (int)uaos_strlen(pbuf);
    for (int i = pl; i < 3; i++) put_c(' ');
    put_s(pbuf); put_s("%  ");
    put_s(mount);
    put_c('\n');
}

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"human-readable", 'h', no_argument},
        {"kibibytes",      'k', no_argument},
        {"inodes",         'i', no_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "hki", long_opts, &li)) != -1) {
        switch (opt) {
            case 'h': opt_human = 1; break;
            case 'k': opt_kibi = 1; break;
            case 'i': opt_inodes = 1; break;
            default:  return 1;
        }
    }

    if (opt_inodes) {
        put_line("Filesystem      Inodes  IUsed  IFree IUse% Mounted on");
    } else {
        put_line("Filesystem     1K-blocks     Used Available Use% Mounted on");
    }

    /* iterate over all mounted volumes */
    long nmounts = uaos_getmountcount();
    for (int i = 0; i < nmounts; i++) {
        char name[32];
        if (uaos_getmountname(i, name, sizeof(name)) > 0) {
            char mount[32];
            uaos_strcpy(mount, name);
            int nl = (int)uaos_strlen(name);
            if (name[nl - 1] != ':') { mount[nl] = ':'; mount[nl + 1] = '\0'; }
            print_volume(name, mount);
        }
    }
    return 0;
}
