---
type: Kernel Library
title: Other AmigaOS Libraries and Devices
description: Native thunk implementations of utility.library, mathffp.library, locale.library, ixemul.library, and device stubs in UAOS.
resource: /kernel/exec/
tags: [utility, mathffp, locale, ixemul, console, keyboard, timer, m68k, thunking]
timestamp: 2026-06-24T17:00:00Z
---

# Other AmigaOS Libraries and Devices

UAOS provides a growing set of native AmigaOS-compatible libraries and devices for emulated M68k tasks. This page covers the smaller libraries and device stubs that do not have dedicated pages.

## utility.library (`kernel/exec/utility_lib.c`)

String, memory, and tag-list helpers.

| Function | Status | Notes |
|---|---|---|
| `StrIcmp` / `StrNicmp` | Implemented | Case-insensitive string comparison. |
| `UCStr` / `LCStr` | Implemented | Convert strings to upper/lower case. |
| `SMult32` / `UMult32` | Implemented | 32×32 multiply and 32-bit scaling. |
| `NextTagItem` / `GetTagData` | Implemented | Walk and query AmigaOS `TagItem` lists. |
| `AllocItem` / `FreeItem` | Stub | Reserved for future use. |
| `DateMatch` | Stub | Reserved for future use. |

Tag list parsing follows the AmigaOS convention: `TAG_DONE`, `TAG_MORE`, `TAG_IGNORE`, `TAG_JUMP`, and `TAG_END` are recognised and skipped as needed.

## mathffp.library (`kernel/exec/mathffp_lib.c`)

Software single-precision floating-point library.

| Function | Status | Notes |
|---|---|---|
| `SPAdd`, `SPSub`, `SPMul`, `SPDiv` | Implemented | Basic IEEE 754 arithmetic. |
| `SPCmp`, `SPNeg`, `SPAbs` | Implemented | Comparison and sign operations. |
| `SPFix`, `SPFlt` | Implemented | Float↔integer conversion. |
| `SPSqrt`, `SPLog`, `SPExp`, `SPSin`, `SPCos`, `SPTan`, `SPAtan`, `SPAsin`, `SPAcos` | Stub | Return NaN or infinity because freestanding UAOS has no math library. |

## locale.library (`kernel/exec/locale_lib.c`)

Localization and date formatting.

| Function | Status | Notes |
|---|---|---|
| `OpenLocale` / `CloseLocale` | Implemented | Returns a default US/English locale. |
| `FormatDate` | Implemented | Parses `%a`, `%A`, `%b`, `%B`, `%d`, `%m`, `%Y`, `%H`, `%M`, `%S`, `%p`, and other common format specifiers. |
| `GetLocaleStr` | Implemented | Looks up month/day names and other locale strings. |
| `IsUpper`, `IsLower`, `IsAlpha`, `IsDigit`, `IsSpace`, `IsPunct` | Implemented | Character classification helpers. |

`FormatDate` converts a `DateStamp` to a Unix timestamp using the current NTP/RTC epoch before formatting.

## ixemul.library (`kernel/exec/ixemul_lib.c`)

Unix compatibility layer. **All functions in this library are currently stubs** that print a diagnostic to stderr and return an error or safe default. They exist so that Amiga binaries linked against `ixemul.library` can load and report missing functionality rather than crashing on an unresolved symbol.

Stubbed categories include: file I/O (`open`, `read`, `write`, `lseek`, `ioctl`, `stat`, `fstat`), directory I/O (`opendir`, `readdir`, `chdir`, `getcwd`), memory (`malloc`, `free`, `calloc`, `realloc`, `strdup`), environment (`getenv`, `setenv`, `putenv`), process control (`fork`, `execve`, `wait`, `waitpid`, `kill`, `signal`), and IPC (`pipe`, `dup`, `dup2`, `fcntl`).

## console.device (`kernel/exec/console_device.c`)

AmigaOS console I/O device. **Currently stubbed**: `OpenDevice`, `CloseDevice`, `BeginIO`, `AbortIO`, `RawKey`, `Read`, `Write`, and `RawWrite` all print a diagnostic and return a default value. Console output from M68k programs is currently handled by the `dos.library` Output/Write path rather than `console.device`.

## keyboard.device (`kernel/exec/keyboard_device.c`)

AmigaOS keyboard input device.

| Function | Status | Notes |
|---|---|---|
| `OpenDevice` / `CloseDevice` | Stub | Returns success. |
| `BeginIO` / `AbortIO` | Stub | Returns success. |
| `Read` | Implemented | Reads a translated character from the PS/2 keyboard ring buffer via `PS2Kbd_GetChar()`. |
| `Write` | Stub | LED control (TODO). |
| `RawKey` | Stub | Raw keycode read (TODO). |

The device passes `InputEvent` structures to the guest but currently only character input is wired to the PS/2 driver.

## timer.device (`kernel/exec/timer_device.c`)

AmigaOS timing device.

| Function | Status | Notes |
|---|---|---|
| `OpenDevice` / `CloseDevice` | Implemented | Opens/closes the device unit. |
| `BeginIO` | Implemented | Queues `TR_ADDREQUEST` timer requests in a 32-entry queue. |
| `GetSysTime` | Implemented | Returns the current time using the NTP/RTC epoch. |
| `EClockUpdate` / `ReadEClock` | Implemented | E-clock counter in microseconds. |
| `AddTime` / `SubTime` / `CmpTime` | Implemented | `timeval` arithmetic. |

When a queued timer request expires, the device signals the requesting task so it can `Wait()` on the timer signal bit.

## ROM Module Registration

All of the above libraries and devices are registered at boot by `kernel/exec/rom_modules.c` via `UAOS_ROM_RegisterAll()`. The ROM module table supports up to 64 modules and maps names to version, base, and native function tables. The registered set also includes `exec.library`, `dos.library`, `graphics.library`, `intuition.library`, `bsdsocket.library`, and `workbench.library`.
