/* unlink.c — GNU coreutils 'unlink' for UAOS gnu: layer
 *
 * Call the unlink function to remove the specified file.
 *   unlink FILE
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = { {NULL, 0, 0} };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "", long_opts, &li)) != -1) {
        return 1;
    }
    int nops = uaos_operands_count(argc);
    if (nops != 1) { put_line("unlink: usage: unlink FILE"); return 1; }
    const char *fname = uaos_operand(argc, argv, 0);
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(fname, path, sizeof(path));
    if (uaos_delete(path) != 0) {
        put_s("unlink: cannot unlink '"); put_s(fname); put_line("'");
        return 1;
    }
    return 0;
}
