/* makedir.c — UAOS x86-64 userspace 'makedir' command
 *
 * AmigaDOS C:MakeDir — create a directory.
 *   makedir <path>
 */

#include "uaos_cmd.h"

int main(int argc, const char **argv)
{
    if (argc < 2 || !argv[1][0]) {
        put_line("Usage: makedir <path>");
        return 5;
    }

    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(argv[1], path, sizeof(path));

    long rc = uaos_mkdir(path);
    if (rc == 0) {
        put_s("Created: ");
        put_line(path);
        return 0;
    }
    put_line("Failed to create directory.");
    return 5;
}
