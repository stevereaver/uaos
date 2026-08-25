/* env.c — GNU coreutils 'env' for UAOS gnu: layer
 *
 * Run a command in a modified environment.
 *   env [OPTION]... [-] [NAME=VALUE]... [COMMAND [ARG]...]
 * Options: -i, --ignore-environment, -u NAME, --unset=NAME, -C DIR, --chdir=DIR
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

static int opt_ignore = 0;

int main(int argc, const char **argv)
{
    static const uaos_long_opt_t long_opts[] = {
        {"ignore-environment", 'i', no_argument},
        {"unset",              'u', required_argument},
        {"chdir",              'C', required_argument},
        {NULL, 0, 0}
    };
    int li, opt;
    while ((opt = uaos_getopt_long(argc, argv, "iu:C:", long_opts, &li)) != -1) {
        switch (opt) {
            case 'i': opt_ignore = 1; break;
            case 'u': break;
            case 'C': break;
            default:  return 1;
        }
    }

    /* skip NAME=VALUE assignments */
    int first_cmd = g_optind;
    while (first_cmd < argc) {
        const char *arg = argv[first_cmd];
        const char *eq = uaos_strchr(arg, '=');
        if (!eq) break;
        first_cmd++;
    }

    if (first_cmd >= argc) {
        /* no command — print environment (empty for now) */
        return 0;
    }

    /* run the command */
    const char *cmd = argv[first_cmd];
    char path[UAOS_CMD_PATH_MAX];
    cmd_make_abs(cmd, path, sizeof(path));

    int n_cmd_args = argc - first_cmd - 1;
    const char **cmd_argv = (const char **)uaos_alloc((n_cmd_args + 2) * sizeof(char *));
    if (!cmd_argv) return 1;
    cmd_argv[0] = cmd;
    for (int i = 1; i <= n_cmd_args; i++)
        cmd_argv[i] = argv[first_cmd + i];
    cmd_argv[n_cmd_args + 1] = NULL;

    long pid = uaos_spawn(path, cmd_argv);
    if (pid < 0) {
        put_s("env: '"); put_s(cmd); put_line("': No such file or directory");
        return 127;
    }
    long status = uaos_wait();
    return (int)status;
}
