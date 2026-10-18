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

#define CMD(n, f)       { (n), (f), NULL, NULL }
#define CMDT(n, f, t)   { (n), (f), (t), NULL }
#define CMDH(n, f, h)   { (n), (f), NULL, (h) }
#define CMDTH(n,f,t,h)  { (n), (f), (t), (h) }

typedef struct {
    const char  *name;      /* lowercase canonical name matching C: stub */
    NativeCmdFn  fn;
    const char  *template;  /* AmigaDOS template string, or NULL */
    const char  *desc;      /* one-line help text, or NULL */
} NativeCmdEntry;

/* help and tab-completion both iterate this table — there is intentionally
 * no second hand-maintained command list to drift out of sync (UAOS-279). */
static const NativeCmdEntry k_native_cmds[] = {
    CMDH ("version",    Cmd_Version  , "OS version info"),
    CMDH ("mem",        Cmd_Mem      , "memory information"),
    CMDH ("cpu",        Cmd_Cpu      , "CPU information"),
    CMDH ("libs",       Cmd_Libs     , "loaded kernel libraries"),
    CMDH ("clear",      Cmd_Clear    , "clear the shell window"),
    CMDH ("reboot",     Cmd_Reboot   , "warm reboot"),
    CMDH ("pwd",        Cmd_Pwd      , "print working directory"),
    CMDH ("info",       Cmd_Info     , "show mounted disks/volumes"),
    CMDH ("date",       Cmd_Date     , "show/set date and time"),
    CMDH ("which",      Cmd_Which    , "which <cmd> — locate a command"),
    CMDH ("disks",      Cmd_Disks    , "list detected block devices"),
    CMDH ("fdisk",      Cmd_Fdisk    , "fdisk <device> — partition a block device"),
    CMDH ("format",     Cmd_Format   , "format <dev> [fs] — format a partition"),
    CMDTH("fsck",       Cmd_Fsck,
         "DEVICE,LIST/S,INFO/S,CHECK/S,REPAIR/S,FIX/S,INTERACTIVE/S,"
         "VERBOSE/S,SURFACE/S,DUMP/K/N,ALL/S",
         "filesystem check/repair tool"),
    CMDH ("pointer",    Cmd_Pointer  , "open pointer preferences"),
    CMDH ("run",        Cmd_Run      , "run <cmd> [args] — detached background run"),
    CMDH ("wbrun",      Cmd_WBRun    , "launch a program with Workbench args"),
    CMDH ("assign",     Cmd_Assign   , "assign [name tgt] — create/list assigns"),
    CMDH ("execute",    Cmd_Execute  , "execute <script> — run a script file"),
    CMDH ("loadwb",     Cmd_LoadWB   , "launch Workbench desktop"),
    CMDH ("calculator", Cmd_CalcWin  , "open calculator window"),
    CMDH ("ifconfig",   Cmd_Ifconfig , "ifconfig [opts] — network interface config"),
    CMDH ("ping",       Cmd_Ping     , "ping <host> — ICMP echo + RTT"),
    CMDH ("route",      Cmd_Route    , "route table show/add/del"),
    CMDH ("nslookup",   Cmd_Nslookup , "nslookup <host> — DNS resolution"),
    CMDH ("ntpd",       Cmd_Ntpd     , "NTP time sync daemon"),
    CMDH ("netstart",   Cmd_Netstart , "bring up the network stack"),
    CMDH ("netstop",    Cmd_Netstop  , "shut down the network stack"),
    CMDH ("clock",      Cmd_ClockWin , "open clock window"),
    CMDH ("vim",        Cmd_Vim      , "vim <file> — inline text editor"),
    CMDH ("newcli",     Cmd_NewCLI   , "open a new shell window"),
    CMDH ("newshell",   Cmd_NewCLI   , "alias for newcli"),
    CMDH ("ask",        Cmd_Ask      , "ask <prompt> — read a line of input"),
    CMDH ("resident",   Cmd_Resident , "resident list management"),
    CMDH ("ps",         Cmd_Ps       , "list running tasks"),
    CMDH ("netinfo",    Cmd_NetInfo  , "network interfaces/ARP/DNS summary"),
    CMDH ("wait",       Cmd_Wait     , "wait <n> [SEC|MSEC|TICKS]"),
    CMDH ("prompt",     Cmd_Prompt   , "prompt <str> — set shell prompt"),
    CMDH ("stack",      Cmd_Stack    , "stack [n] — show/set CLI stack size"),
    CMDH ("why",        Cmd_Why      , "explain last command's return code"),
    CMDH ("failat",     Cmd_Failat   , "failat [n] — script failure threshold"),
    CMDH ("quit",       Cmd_Quit     , "quit a running script"),
    CMDH ("endcli",     Cmd_EndCLI   , "close this shell"),
    CMDH ("relabel",    Cmd_Relabel  , "relabel <dev> <name> — set volume label"),
    CMDH ("getenv",     Cmd_GetEnv   , "getenv <name> — print env variable"),
    CMDH ("unset",      Cmd_UnSet    , "unset <name> — remove env variable"),
    CMDH ("jobs",       Cmd_Jobs     , "list background jobs"),
    CMDTH("install",    Cmd_Install,       "DEVICE/A,NOBOOT/S",
         "install <dev> — write a boot block"),
    CMDH ("diskchange", Cmd_DiskChange, "diskchange <dev> — notify media change"),
    CMDH ("dismount",   Cmd_Dismount,  "dismount <dev> — unmount a volume"),
    CMDH ("adf",        Cmd_Adf,       "adf <file> — mount an ADF floppy image"),
    CMDH ("addbuffers", Cmd_AddBuffers,"addbuffers <dev> [n] — grow fs cache"),
    CMDTH("requestchoice",Cmd_RequestChoice, "TITLE/A,BODY/A,BUTTON/M",
         "open a choice requester"),
    CMDTH("requestfile",Cmd_RequestFile,"TITLE/K,DRAWER/K,FILE/K,PATTERN/K,PUBSCREEN/K",
         "open a file requester"),
    CMDTH("changetaskpri",Cmd_ChangeTaskPri,"PRI/A/N,TASK/K",
         "change a task's priority"),
    CMDTH("status",     Cmd_Status,        "FULL/S,TCB/S,CLI/S",
         "CLI process status"),
    CMDH ("rx",         Cmd_Rx,            "run an ARexx/REXX script"),
    CMDTH("telnetd",    Cmd_Telnetd,       "PORT/K/N,STOP/S,STATUS/S",
         "telnet shell service [PORT=n|STOP|STATUS]"),
    CMDH ("strace",     Cmd_Strace,        "strace [-o] — syscall/dos call trace"),
    CMDH ("klog",       Cmd_Klog,          "klog [s,l=level] — kernel log levels"),
    CMDH ("debug",      Cmd_Klog,          "alias for klog"),
    CMDH ("dmesg",      Cmd_Dmesg,         "dmesg [subsys|lvl] — kernel log dump"),
    CMDH ("irqstat",    Cmd_Irqstat,       "irqstat [sec|NOW] — per-vector IRQ counts"),
    CMDH ("usbdiag",    Cmd_Usbdiag,       "usbdiag [sec] — UHCI register dump/sampler"),
    CMDH ("crash",      Cmd_Crash,         "trigger a test kernel panic"),
    CMDH ("memcheck",   Cmd_Memcheck,      "memcheck [on|off|test] — heap guard debugging"),
    CMDH ("chiptrace",  Cmd_Chiptrace,     "chiptrace [class|pc] — custom-chip tracer"),
    CMDH ("taskdump",   Cmd_Taskdump,      "taskdump <task> — saved frame/registers"),
    CMDH ("taskstat",   Cmd_Taskstat,      "taskstat [<sec>|NOW] — per-task CPU% + switches"),
    CMDH ("watchdog",   Cmd_Watchdog,      "kernel stall watchdog status"),
    CMDH ("ports",      Cmd_Ports,         "MsgPort queue dump"),
    CMDH ("timers",     Cmd_Timers,        "timer.device pending-request dump"),
    CMDH ("handles",    Cmd_Handles,       "open-file/lock/handler queue dump"),
    CMDH ("netstat",    Cmd_Netstat,       "TCP/UDP socket table dump"),
    CMDH ("diskdiag",   Cmd_Diskdiag,      "diskdiag [dev] — storage-path stage dump"),
    CMDH ("pciscan",    Cmd_Pciscan,       "PCI device enumeration"),
    CMDH ("irqroute",   Cmd_Irqroute,      "irqroute [BDF] — INTx/MSI route decode"),
    CMDH ("audiotest",  Cmd_Audiotest,     "audio output test tones"),
    CMDH ("volume",     Cmd_Volume,        "volume [0-100] — master volume"),
    CMDH ("hdadump",    Cmd_HdaDump,       "HDA codec widget dump"),
    CMDH ("peek",       Cmd_Peek,          "peek <addr> — physical memory read"),
    CMDH ("poke",       Cmd_Poke,          "poke <addr> <val> — physical memory write"),
    CMDH ("irqaudit",   Cmd_Irqaudit,      "Disable/Forbid critical-section audit"),
    CMDH ("sercon",     Cmd_Sercon,        "serial console bridge"),
    CMDH ("tickcheck",  Cmd_Tickcheck,     "PIT/TSC/RTC drift + IRQ latency"),
    CMDH ("etrace",     Cmd_Etrace,        "kernel event trace ring"),
    CMDH ("prof",       Cmd_Prof,          "sampling profiler"),
    CMDH ("failalloc",  Cmd_Failalloc,     "deterministic AllocMem fault injection"),
    CMDH ("pktmon",     Cmd_Pktmon,        "packet capture to pcap"),
    CMDH ("screenshot", Cmd_Screenshot,    "capture screen to JPG"),
    /* Prefs editors (stubs — Phase 3) */
    CMDH ("screenmode", Cmd_ScreenMode, "screen-mode prefs editor"),
    CMDH ("font",       Cmd_Font,       "font prefs editor"),
    CMDH ("icontrol",   Cmd_IControl,   "icon prefs editor"),
    CMDH ("input",      Cmd_Input,      "input prefs editor"),
    CMDH ("palette",    Cmd_Palette,    "palette prefs editor"),
    CMDH ("wbpattern",  Cmd_WBPattern,  "workbench-pattern prefs editor"),
    CMDH ("serial",     Cmd_Serial,     "serial prefs editor"),
    CMDH ("printer",    Cmd_Printer,    "printer prefs editor"),
    CMDH ("time",       Cmd_PrefsTime,  "time prefs editor"),
    CMDH ("locale",     Cmd_PrefsLocale,"locale prefs editor"),
    /* Tools & Commodities */
    CMDH ("exchange",   Cmd_Exchange,   "CX commodity manager"),
    CMDH ("blanker",    Cmd_Blanker,    "screen blanker control"),
    CMDH ("backlight",  Cmd_Backlight,  "display backlight control"),
    /* Printing & CrossDOS */
    CMDH ("print",      Cmd_Print,      "print <file> — printer output"),
    CMDH ("crossdos",   Cmd_CrossDOS,   "FAT filesystem exchange"),
    /* Editor */
    CMDH ("ed",         Cmd_Ed,         "ed <file> — line editor"),
    /* Help */
    CMDH ("guide",      Cmd_Guide,      "open the AmigaGuide help viewer"),
    /* Shell-state commands — alias/unalias/path/skip/lab are normally
     * intercepted as built-ins or script keywords before the native
     * table; these entries let the C: binaries resolve too. */
    CMDH ("runback",    Cmd_RunBack,    "run a command in the background"),
    CMDH ("alias",      Cmd_Alias,      "alias [name cmd] — command aliases"),
    CMDH ("unalias",    Cmd_UnAlias,    "remove an alias"),
    CMDH ("path",       Cmd_Path,       "path [dirs..] [ADD] — command search path"),
    CMDH ("skip",       Cmd_Skip,       "script: skip to label/lines"),
    CMDH ("lab",        Cmd_Lab,        "script: define a label"),
    CMDH ("resload",    Cmd_ResLoad,    "resload <cmd> — load into resident list"),
    { NULL, NULL, NULL, NULL }
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
                s = " [args='";
                while (*s && j < 127) msg[j++] = *s++;
                s = args ? args : "(null)";
                while (*s && j < 127) msg[j++] = *s++;
                msg[j] = '\0';
                if (j < 126) { msg[j++] = '\''; msg[j] = '\0'; }
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

int NativeCmd_Count(void)
{
    int n = 0;
    while (k_native_cmds[n].name) n++;
    return n;
}

const char *NativeCmd_NameAt(int i)
{
    return (i >= 0 && i < NativeCmd_Count()) ? k_native_cmds[i].name : NULL;
}

/* Append "name" right-aligned to width into buf (no libc sprintf). */
static void nc_pad(char *buf, const char *name, int width)
{
    int i = 0;
    while (name[i] && i < 60) { buf[i] = name[i]; i++; }
    while (i < width) buf[i++] = ' ';
    buf[i] = '\0';
}

static void nc_cat(char *buf, const char *s, int max)
{
    int i = 0;
    while (buf[i]) i++;
    while (*s && i < max - 1) buf[i++] = *s++;
    buf[i] = '\0';
}

void NativeCmd_PrintHelp(void *shell, void (*print)(void *, const char *),
                         const char *name)
{
    if (!print) return;
    if (name) {
        /* help <cmd> — usage line for one command */
        for (int i = 0; k_native_cmds[i].name; i++) {
            if (!nc_ieq(k_native_cmds[i].name, name)) continue;
            char line[160];
            nc_pad(line, k_native_cmds[i].name, 16);
            nc_cat(line, k_native_cmds[i].desc ? k_native_cmds[i].desc : "",
                   sizeof(line));
            print(shell, line);
            if (k_native_cmds[i].template) {
                char tl[160];
                tl[0] = '\0';
                nc_cat(tl, "  template: ", sizeof(tl));
                nc_cat(tl, k_native_cmds[i].template, sizeof(tl));
                print(shell, tl);
            }
            return;
        }
        char msg[96];
        msg[0] = '\0';
        nc_cat(msg, "help: no native command '", sizeof(msg));
        nc_cat(msg, name, sizeof(msg));
        nc_cat(msg, "'", sizeof(msg));
        print(shell, msg);
        return;
    }
    for (int i = 0; k_native_cmds[i].name; i++) {
        char line[160];
        nc_pad(line, k_native_cmds[i].name, 16);
        nc_cat(line, k_native_cmds[i].desc ? k_native_cmds[i].desc : "",
               sizeof(line));
        print(shell, line);
    }
}
