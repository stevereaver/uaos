/* rename.c — UAOS x86-64 userspace 'rename' command
 *
 * AmigaDOS C:Rename — rename or move a file.
 *   rename <old> <new>
 */

#include "uaos_cmd.h"

int main(int argc, const char **argv)
{
    if (argc < 3 || !argv[1][0] || !argv[2][0]) {
        put_line("Usage: rename <old> <new>");
        return 5;
    }

    char src[UAOS_CMD_PATH_MAX], dst[UAOS_CMD_PATH_MAX];
    cmd_make_abs(argv[1], src, sizeof(src));
    cmd_make_abs(argv[2], dst, sizeof(dst));

    if (uaos_rename(src, dst) == 0) {
        put_line("Renamed successfully.");
        return 0;
    }
    put_line("Rename failed.");
    return 5;
}
