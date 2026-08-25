/* false.c — GNU coreutils 'false' for UAOS gnu: layer
 *
 * Exit with a status code indicating failure.
 *   false [ignored command line arguments]
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

int main(int argc, const char **argv)
{
    (void)argc; (void)argv;
    return 1;
}
