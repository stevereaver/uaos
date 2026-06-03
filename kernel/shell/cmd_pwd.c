/* cmd_pwd.c — C:pwd — print working directory */

#include "cmd_internal.h"

void Cmd_Pwd(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    PRINT(ctx->cwd);
}
