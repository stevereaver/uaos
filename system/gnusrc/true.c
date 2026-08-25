/* true.c — GNU coreutils 'true' for UAOS gnu: layer
 *
 * Exit with a status code indicating success.
 *   true [ignored command line arguments]
 */

#include "uaos_cmd.h"
#include "uaos_getopt.h"

int main(int argc, const char **argv)
{
    (void)argc; (void)argv;
    return 0;
}
