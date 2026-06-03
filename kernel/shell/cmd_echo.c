/* cmd_echo.c — C:echo — print text to the shell */

#include "cmd_internal.h"

void Cmd_Echo(NativeCmdCtx *ctx, const char *args)
{
    PRINT(args && *args ? args : "");
}
