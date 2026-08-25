/* yes.c — GNU coreutils 'yes' for UAOS gnu: layer
 *
 * Output a string repeatedly until killed.
 *   yes [STRING]...
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

int main(int argc, const char **argv)
{
    /* yes doesn't use getopt — everything is the string to repeat */
    const char *str = NULL;
    char buf[256];
    if (argc > 1) {
        buf[0] = '\0';
        for (int i = 1; i < argc; i++) {
            if (i > 1) uaos_strlcat(buf, " ", sizeof(buf));
            uaos_strlcat(buf, argv[i], sizeof(buf));
        }
        str = buf;
    }
    for (;;) {
        if (str) { put_s(str); put_c('\n'); }
        else { put_c('y'); put_c('\n'); }
    }
    return 0;
}
