/* native_cmd.c — UAOS Native Command Registry
 *
 * Maps command names (as found on C:) to their native x86 handler functions.
 * Case-insensitive lookup so "DIR", "Dir", and "dir" all resolve correctly.
 */

#include "native_cmd.h"
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Command table
 * ------------------------------------------------------------------------- */

typedef struct {
    const char  *name;   /* lowercase canonical name matching C: stub */
    NativeCmdFn  fn;
} NativeCmdEntry;

static const NativeCmdEntry k_native_cmds[] = {
    { "version", Cmd_Version  },
    { "mem",     Cmd_Mem      },
    { "libs",    Cmd_Libs     },
    { "clear",   Cmd_Clear    },
    { "reboot",  Cmd_Reboot   },
    { "dir",     Cmd_Dir      },
    { "makedir", Cmd_Makedir  },
    { "delete",  Cmd_Delete   },
    { "type",    Cmd_Type     },
    { "copy",    Cmd_Copy     },
    { "rename",  Cmd_Rename   },
    { "pwd",     Cmd_Pwd      },
    { "echo",    Cmd_Echo     },
    { "protect", Cmd_Protect  },
    { "attr",    Cmd_Attr     },
    { "info",    Cmd_Info     },
    { "date",    Cmd_Date     },
    { "which",   Cmd_Which    },
    { "disks",   Cmd_Disks    },
    { "fdisk",   Cmd_Fdisk    },
    { "format",  Cmd_Format   },
    { "pointer", Cmd_Pointer  },
    { "run",     Cmd_Run      },
    { "assign",  Cmd_Assign   },
    { "execute", Cmd_Execute  },
    { "loadwb",  Cmd_LoadWB   },
    { "calculator", Cmd_CalcWin  },
    { "ifconfig",   Cmd_Ifconfig  },
    { "ping",       Cmd_Ping      },
    { "route",      Cmd_Route     },
    { "nslookup",   Cmd_Nslookup  },
    { "ntpd",       Cmd_Ntpd      },
    { "clock",      Cmd_ClockWin  },
    { NULL,         NULL          }
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
        if (nc_ieq(k_native_cmds[i].name, name)) {
            k_native_cmds[i].fn(ctx, args);
            return 0;
        }
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
