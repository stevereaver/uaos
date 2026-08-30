/* native_cmd.c — UAOS Native Command Registry
 *
 * Maps command names (as found on C:) to their native x86 handler functions.
 * Case-insensitive lookup so "DIR", "Dir", and "dir" all resolve correctly.
 *
 * Commands may optionally declare an AmigaDOS-style template.  When a template
 * is present, NativeCmd_Run parses the argument string automatically before
 * calling the handler, and places the result in ctx->template.
 */

#include "native_cmd.h"
#include "cmd_template.h"
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Command table
 * ------------------------------------------------------------------------- */

#define CMD(n, f)     { (n), (f), NULL }
#define CMDT(n, f, t) { (n), (f), (t) }

typedef struct {
    const char  *name;      /* lowercase canonical name matching C: stub */
    NativeCmdFn  fn;
    const char  *template;  /* AmigaDOS template string, or NULL */
} NativeCmdEntry;

static const NativeCmdEntry k_native_cmds[] = {
    CMD ("version",    Cmd_Version  ),
    CMD ("mem",        Cmd_Mem      ),
    CMD ("libs",       Cmd_Libs     ),
    CMD ("clear",      Cmd_Clear    ),
    CMD ("reboot",     Cmd_Reboot   ),
    CMD ("pwd",        Cmd_Pwd      ),
    CMD ("info",       Cmd_Info     ),
    CMD ("date",       Cmd_Date     ),
    CMD ("which",      Cmd_Which    ),
    CMD ("disks",      Cmd_Disks    ),
    CMD ("fdisk",      Cmd_Fdisk    ),
    CMD ("format",     Cmd_Format   ),
    CMD ("pointer",    Cmd_Pointer  ),
    CMD ("run",        Cmd_Run      ),
    CMD ("assign",     Cmd_Assign   ),
    CMD ("execute",    Cmd_Execute  ),
    CMD ("loadwb",     Cmd_LoadWB   ),
    CMD ("calculator", Cmd_CalcWin  ),
    CMD ("ifconfig",   Cmd_Ifconfig ),
    CMD ("ping",       Cmd_Ping     ),
    CMD ("route",      Cmd_Route    ),
    CMD ("nslookup",   Cmd_Nslookup ),
    CMD ("ntpd",       Cmd_Ntpd     ),
    CMD ("netstart",   Cmd_Netstart ),
    CMD ("netstop",    Cmd_Netstop  ),
    CMD ("clock",      Cmd_ClockWin ),
    CMD ("vim",        Cmd_Vim      ),
    CMD ("newcli",     Cmd_NewCLI   ),
    CMD ("newshell",   Cmd_NewCLI   ),  /* alias */
    CMD ("ask",        Cmd_Ask      ),
    CMD ("resident",   Cmd_Resident ),
    CMD ("ps",         Cmd_Ps       ),
    CMD ("netinfo",    Cmd_NetInfo  ),
    CMD ("wait",       Cmd_Wait     ),
    CMD ("prompt",     Cmd_Prompt   ),
    CMD ("stack",      Cmd_Stack    ),
    CMD ("why",        Cmd_Why      ),
    CMD ("failat",     Cmd_Failat   ),
    CMD ("quit",       Cmd_Quit     ),
    CMD ("endcli",     Cmd_EndCLI   ),
    CMD ("relabel",    Cmd_Relabel  ),
    CMD ("getenv",          Cmd_GetEnv        ),
    CMD ("unset",           Cmd_UnSet         ),
    CMD ("jobs",            Cmd_Jobs          ),
    CMDT("install",         Cmd_Install,       "DEVICE/A,NOBOOT/S" ),
    CMD ("diskchange",      Cmd_DiskChange    ),
    CMD ("addbuffers",      Cmd_AddBuffers    ),
    CMDT("requestchoice",   Cmd_RequestChoice, "TITLE/A,BODY/A,BUTTON/M" ),
    CMDT("requestfile",     Cmd_RequestFile,   "TITLE/K,DRAWER/K,FILE/K,PATTERN/K,PUBSCREEN/K" ),
    CMDT("changetaskpri",   Cmd_ChangeTaskPri, "PRI/A/N,TASK/K" ),
    CMDT("status",          Cmd_Status,        "FULL/S,TCB/S,CLI/S" ),
    CMD ("rx",              Cmd_Rx             ),
    /* Prefs editors (stubs — Phase 3) */
    CMD ("screenmode",      Cmd_ScreenMode  ),
    CMD ("font",            Cmd_Font        ),
    CMD ("icontrol",        Cmd_IControl    ),
    CMD ("input",           Cmd_Input       ),
    CMD ("palette",         Cmd_Palette     ),
    CMD ("wbpattern",       Cmd_WBPattern   ),
    CMD ("serial",          Cmd_Serial      ),
    CMD ("printer",         Cmd_Printer     ),
    CMD ("time",            Cmd_PrefsTime   ),
    CMD ("locale",          Cmd_PrefsLocale ),
    /* Tools & Commodities */
    CMD ("exchange",        Cmd_Exchange    ),
    CMD ("blanker",         Cmd_Blanker     ),
    /* Printing & CrossDOS */
    CMD ("print",           Cmd_Print       ),
    CMD ("crossdos",        Cmd_CrossDOS    ),
    /* Editor */
    CMD ("ed",              Cmd_Ed          ),
    /* Help */
    CMD ("guide",           Cmd_Guide       ),
    { NULL, NULL, NULL }
};

/* -------------------------------------------------------------------------
 * Helpers (no libc)
 * ------------------------------------------------------------------------- */

static int nc_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* Case-insensitive equality */
static int nc_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (nc_tolower((unsigned char)*a) != nc_tolower((unsigned char)*b))
            return 0;
        a++; b++;
    }
    return *a == *b;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

int NativeCmd_Run(const char *name, NativeCmdCtx *ctx, const char *args)
{
    for (int i = 0; k_native_cmds[i].name; i++) {
        if (!nc_ieq(k_native_cmds[i].name, name))
            continue;

        if (k_native_cmds[i].template) {
            CmdTemplateResult tr;
            CmdTemplate_Parse(k_native_cmds[i].template, &tr);
            if (tr.error[0]) {
                char msg[128];
                int j = 0;
                const char *s = "Template parse error: ";
                while (*s && j < 127) msg[j++] = *s++;
                s = tr.error;
                while (*s && j < 127) msg[j++] = *s++;
                msg[j] = '\0';
                if (ctx->print)
                    ctx->print(ctx->shell, msg);
                if (ctx->set_rc)
                    ctx->set_rc(ctx->shell_extra, 20);
                return 0;
            }

            CmdTemplate_MatchArgs(&tr, args);
            if (tr.error[0]) {
                char msg[128];
                int j = 0;
                const char *s = "Bad args: ";
                while (*s && j < 127) msg[j++] = *s++;
                s = tr.error;
                while (*s && j < 127) msg[j++] = *s++;
                msg[j] = '\0';
                if (ctx->print)
                    ctx->print(ctx->shell, msg);
                if (ctx->set_rc)
                    ctx->set_rc(ctx->shell_extra, 20);
                return 0;
            }

            ctx->template = &tr;
            k_native_cmds[i].fn(ctx, args);
            ctx->template = NULL;
        } else {
            k_native_cmds[i].fn(ctx, args);
        }
        return 0;
    }
    return -1;
}

int NativeCmd_Exists(const char *name)
{
    for (int i = 0; k_native_cmds[i].name; i++) {
        if (nc_ieq(k_native_cmds[i].name, name))
            return 1;
    }
    return 0;
}
