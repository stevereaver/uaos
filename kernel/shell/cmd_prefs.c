/*
 * cmd_prefs.c — Preferences editor command dispatchers
 *
 * Launches GUI preferences editors when invoked from the shell.
 */

#include "native_cmd.h"
#include "../display/prefs_win.h"

void Cmd_ScreenMode(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    ScreenModePrefs_Show();
    if (ctx->print) ctx->print(ctx->shell, "ScreenMode preferences opened.");
}

void Cmd_Font(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    FontPrefs_Show();
    if (ctx->print) ctx->print(ctx->shell, "Font preferences opened.");
}

void Cmd_IControl(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    IControlPrefs_Show();
    if (ctx->print) ctx->print(ctx->shell, "IControl preferences opened.");
}

void Cmd_Input(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    InputPrefs_Show();
    if (ctx->print) ctx->print(ctx->shell, "Input preferences opened.");
}

void Cmd_Palette(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    PalettePrefs_Show();
    if (ctx->print) ctx->print(ctx->shell, "Palette preferences opened.");
}

void Cmd_WBPattern(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    WBPatternPrefs_Show();
    if (ctx->print) ctx->print(ctx->shell, "WBPattern preferences opened.");
}

void Cmd_Serial(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    SerialPrefs_Show();
    if (ctx->print) ctx->print(ctx->shell, "Serial preferences opened.");
}

void Cmd_Printer(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    PrinterPrefs_Show();
    if (ctx->print) ctx->print(ctx->shell, "Printer preferences opened.");
}

void Cmd_PrefsTime(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    TimePrefs_Show();
    if (ctx->print) ctx->print(ctx->shell, "Time preferences opened.");
}

void Cmd_PrefsLocale(NativeCmdCtx *ctx, const char *args)
{
    (void)args;
    LocalePrefs_Show();
    if (ctx->print) ctx->print(ctx->shell, "Locale preferences opened.");
}
