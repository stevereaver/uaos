---
type: Kernel Library
title: Other AmigaOS Libraries and Devices
description: Native thunk implementations of utility.library, mathffp.library, mathieeesingbas.library, mathtrans.library, locale.library, diskfont.library, ixemul.library, and device stubs in UAOS.
resource: /kernel/exec/
tags: [utility, mathffp, mathieeesingbas, mathtrans, locale, ixemul, console, keyboard, timer, m68k, thunking]
timestamp: 2026-09-23T23:35:38Z
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
| `SPSqrt`, `SPLog`, `SPExp`, `SPSin`, `SPCos`, `SPTan`, `SPAtan`, `SPAsin`, `SPAcos` | Implemented | Call the shared freestanding helpers in `float_math.c` (same code as `mathtrans.library`). `SPLog` is the natural logarithm (ln). |

## mathieeesingbas.library (`kernel/exec/mathieeesingbas_lib.c`)

IEEE 754 single-precision basic floating-point operations. Required by ACE Basic, which opens this library unconditionally at startup.

| Function | Status | Notes |
|---|---|---|
| `SPFix`, `SPFlt` | Implemented | Float↔integer conversion (round toward zero). |
| `SPCmp`, `SPTst` | Implemented | Compare two floats / test against zero. Returns -1/0/+1. |
| `SPAbs`, `SPNeg` | Implemented | Absolute value and negation via sign-bit manipulation. |
| `SPAdd`, `SPSub`, `SPMul`, `SPDiv` | Implemented | Basic IEEE 754 arithmetic using native C float. |

Registered at base address `0x00000070`. Uses the same IEEE 754 conversion helpers as mathffp.library.

## mathtrans.library (`kernel/exec/mathtrans_lib.c`)

IEEE 754 single-precision transcendental functions. Required by ACE Basic, which opens this library unconditionally at startup. The function bodies live in the shared freestanding module `float_math.c` / `float_math.h` (exported as `uaos_sinf`, `uaos_cosf`, `uaos_tanf`, `uaos_sqrtf`, `uaos_expf`, `uaos_logf`, `uaos_asinf`, `uaos_acosf`, `uaos_atanf`, `uaos_floorf`, `uaos_ceilf`, `uaos_powf`), implemented without `<math.h>` using Taylor series and Newton-Raphson iterations. `mathffp.library` calls the same helpers.

| Function | Status | Notes |
|---|---|---|
| `SPSin`, `SPCos`, `SPTan` | Implemented | Taylor series with angle reduction to [-π, π]. |
| `SPSincos` | Implemented | Returns sin in D0, stores cos at A0 pointer. |
| `SPAsin`, `SPAcos`, `SPAtan` | Implemented | Taylor series; asin/acos use atan fallback for |x| > 0.5. |
| `SPExp` | Implemented | Taylor series with range reduction via `e^x = 2^k * e^r`. |
| `SPLog` | Implemented | Natural log via artanh series with mantissa range reduction. |
| `SPLog10` | Implemented | `SPLog * log10(e)`. |
| `SPSqrt` | Implemented | Newton-Raphson iteration: `x_{n+1} = 0.5*(x_n + S/x_n)`. |
| `SPFloor`, `SPCeil` | Implemented | Integer cast with correction for negative values. |
| `SPPow` | Implemented | `SPExp(exp * SPLog(base))` with negative-base handling. |

Registered at base address `0x00000080`.

## locale.library (`kernel/exec/locale_lib.c`)

Localization and date formatting.

| Function | Status | Notes |
|---|---|---|
| `OpenLocale` / `CloseLocale` | Implemented | Returns a default US/English locale. |
| `FormatDate` | Implemented | Parses `%a`, `%A`, `%b`, `%B`, `%d`, `%m`, `%Y`, `%H`, `%M`, `%S`, `%p`, and other common format specifiers. |
| `GetLocaleStr` | Implemented | Looks up month/day names and other locale strings. |
| `IsUpper`, `IsLower`, `IsAlpha`, `IsDigit`, `IsSpace`, `IsPunct` | Implemented | Character classification helpers. |

`FormatDate` converts a `DateStamp` to a Unix timestamp using the current NTP/RTC epoch before formatting.

Catalogs: when no catalog is loaded, `GetCatalogStr` (LVO -72, handled in the generic-library dispatch in `emulation/uaos_m68k_glue.c`) returns the caller's `defaultString` argument — returning NULL instead blanks every application string (observed with OctaMED's ~160 startup string fetches). `OpenCatalogA` may legitimately return NULL when no catalog exists.

## asl.library (generic dispatch)

The file requester is implemented natively on top of the WM (`UAOS_Intuition_AslFileRequest` in `kernel/exec/intuition_lib.c`, dispatched from `emulation/uaos_m68k_glue.c`). It targets V37+ semantics and is sufficient for OctaMED's load/save flows.

| Function | Status | Notes |
|---|---|---|
| `AllocFileRequest` (LVO -30) | Implemented | Zeroed 512-byte block in guest memory; result strings live inside it (drawer at +256, file at +384). V36 alias of `AllocAslRequest(ASL_FileRequest,...)`. |
| `FreeFileRequest` (LVO -36) | Implemented | Frees the block plus any owned ArgList. |
| `RequestFile` (LVO -42) | Implemented | Shares the `AslRequest` path: a0=requester block, a1=taglist. |
| `AllocAslRequest` (LVO -48) | Implemented | Type 0 (`ASL_FileRequest`) accepted; taglist parsed at alloc time and persisted in the block. |
| `FreeAslRequest` (LVO -54) | Implemented | Frees the block plus any owned ArgList/dir-lock. |
| `AslRequest` (LVO -60) | Implemented | Native modal file requester. Re-applies the call-time taglist so per-call overrides work. |
| `AbortAslRequest` (LVO -66) | Implemented | Cancels a pending requester (sets the done flag). |
| `ActivateFileRequest` (LVO -72) | Implemented | V36 noop-compat stub returning success. |

Private per-requester config is kept inside the 512-byte block: requester type +56, Flags1 +60, Flags2 +64, geometry +68..+74, owned `fr_ArgList` pointer/size +76/+80, and effective option bits +96. `fr_LeftEdge/TopEdge/Width/Height` (+22..+28), `fr_NumArgs`/`fr_ArgList` (+32/+36), `fr_UserData` (+40), `fr_Pattern` (+52) match the V36/V44 `FileRequester` layout.

Tag parsing (`frq_apply_tags`) accepts both V36 `ASL_*` and V38 `ASLFR_*` numbering (they share base `TAG_USER+0x80000`): title, window, geometry, initial file/drawer/pattern, OK/Cancel text, FuncFlags (+20), ExtFlags1/2 (+22/+24), SaveMode (+44), MultiSelect (+45), DoPatterns/DoMultiSelect/DrawersOnly (+46..). Control tags are raw utility values (`TAG_IGNORE=1`, `TAG_MORE=2`, `TAG_SKIP=3`, `TAG_JUMP=4` — NOT `TAG_USER`-offset); OctaMED chains its taglists across ~10 `TAG_MORE` hops, so the walker follows up to 16. FuncFlag bits: `FILF_PATGAD`=1, `FILF_MULTISELECT`=8, `FILF_NEWIDCMP`=0x10, `FILF_SAVE`=0x20; ExtFlags1: `FIL1F_NOFILES`=1 (drawers-only picker), `FIL1F_MATCHDIRS`=2.

`AslRequest` opens a modal WM window and blocks the calling M68k task on a signal bit while the EventPump drives input; `UAOS_M68k_DeliverInterrupts()` is pumped inside the wait loop so CIA/audio keep running. Navigation: **Volumes** lists mounted volumes, **Parent** pops a path component, clicking a directory enters it (in drawers-only mode a click *selects* instead), clicking a file selects it (toggles a mark in multi-select), **OK**/**Cancel** finish. Drawer, file and pattern gadgets are editable text fields (click to focus, rawkey→ASCII cooked input, Backspace/Return). The requester window is marked `WM_SetModal` so it can never be buried under a raised window while the guest is blocked.

Filtering honours `ASLFR_Pattern`/`ASL_InitialPattern` via the AmigaDOS pattern matcher (`pattern_match` in `kernel/exec/dos_lib.c`, upgraded to full syntax: `~` negation, `(a|b)` groups, `'c` quoting, `#x` repetition — OctaMED's `~(#?.info|backdrop)` works), `ASLFR_RejectIcons`, `ASLFR_DrawersOnly`, `ASLFR_FilterDrawers`, plus `ASLFR_AcceptPattern`/`ASLFR_RejectPattern`.

Multi-select (`ASLFR_DoMultiSelect`/`FILF_MULTISELECT`) builds a guest `WBArg` array in caller-visible memory: `fr_NumArgs` entries, each `wa_Lock` a real dos lock on the parent drawer (shared locks on `Lock()`-resolved path via `dos_LockPath_glue`) and `wa_Name` pointing into the block. Save mode (`FILF_SAVE`/`ASLFR_SaveMode`) accepts non-existent filenames and keeps the initial file text editable. On accept the result lands in `fr_Drawer`/`fr_File` so the guest reads a full AmigaDOS path.

Verified end-to-end by `tests/qemu_asl_test.py` driving `system/Demos/src/ASLTest.s` (multi-select WBArg opens via `wa_Lock`, save-path creation, delete via `DeleteFile`, cancel→FALSE) and by the OctaMED instrument-load regression `tests/qemu_octamed_iff_test.py`.

One host-side fix this surfaced: `ram_handler.c` treated `VFS_Delete`/`VFS_MkDir` as boolean-success when they return `int` (0=ok/-1=fail), so every guest packet delete/mkdir on `RAM:` reported the result inverted — now `== 0` checks like the FAT/FFS handlers. Also added `FilePart`/`PathPart` dos LVOs (-870/-876) which OctaMED's save flow needs.

## diskfont.library (`kernel/exec/diskfont_lib.c`)

Disk-font loading stub (UAOS-249). diskfont.library has been ROM-resident since Kickstart 2.0, so it registers like the real thing — `OpenLibrary` succeeds — but every font operation reports "no fonts", letting callers (OctaMED) fall back to the built-in topaz font.

|| Function | Status | Notes |
|---|---|---|
|| `OpenDiskFont` (LVO -30) | Stub | Returns NULL — font not found. |
|| `AvailFonts` (LVO -36) | Stub | Writes `afh_NumEntries = 0`, returns success — empty font list. |
|| `NewFontContents` (LVO -42) | Stub | Returns NULL. |
|| `DisposeFontContents` (LVO -48) | Stub | No-op. |
|| `NewScaledDiskFont` (LVO -54) | Stub | Returns NULL. |

## ixemul.library (`kernel/exec/ixemul_lib.c`)

Unix compatibility layer. **All functions in this library are currently stubs** that print a diagnostic to stderr and return an error or safe default. They exist so that Amiga binaries linked against `ixemul.library` can load and report missing functionality rather than crashing on an unresolved symbol.

Stubbed categories include: file I/O (`open`, `read`, `write`, `lseek`, `ioctl`, `stat`, `fstat`), directory I/O (`opendir`, `readdir`, `chdir`, `getcwd`), memory (`malloc`, `free`, `calloc`, `realloc`, `strdup`), environment (`getenv`, `setenv`, `putenv`), process control (`fork`, `execve`, `wait`, `waitpid`, `kill`, `signal`), and IPC (`pipe`, `dup`, `dup2`, `fcntl`).

## Guest device I/O model (UAOS-240)

Guest `IORequest`/`IOStdReq`/`MsgPort` structures live in the requester's big-endian guest RAM window (`IO_DEVICE` +20, `IO_UNIT` +24, `IO_COMMAND` +28, `IO_FLAGS` +30, `IO_ERROR` +31; `IOStdReq`: `io_Actual` +32, `io_Length` +36, `io_Data` +40). `exec_DoIO`/`exec_SendIO` route a request to the device's `BeginIO` vector (generated-base LVO `-42`, or `audio.device`'s dedicated base); `exec_AbortIO` runs the `-48` vector.

Completion is asynchronous: a device that cannot finish inside `BeginIO` sets `IOF_QUEUED` (and clears `IOF_QUICK`), then replies later from its own context (PIT tick, event pump) via the exported `UAOS_Emu_IOReply(ram, io)`. The reply sets `IOF_DONE` + `ln_Type = NT_REPLYMSG`, AddTails the node onto `mn_ReplyPort`, and `Signal()`s `mp_SigTask` with `1 << mp_SigBit` — the same protocol `exec_PutMsg` uses, so `WaitPort`/`Wait`/`GetMsg` observe it. `QUICK` completions skip the port put entirely. `exec_CheckIO` returns the request iff `IOF_QUEUED` has cleared; `exec_WaitIO`/`DoIO` nap on the port's signal bit (flag is the wait condition so port-less completions wake too) and consume the reply node afterwards. Task-exit cleanup (`UAOS_M68k_ReleaseTaskResources`) drops the task's pending device requests via `IODev_DropTaskRequests`/`ConDev_DropTaskRequests`/`KbdDev_DropTaskRequests` so completions never fire into a dead RAM window.

## console.device (`kernel/exec/console_device.c`)

AmigaOS console I/O device — real BeginIO since UAOS-240. `CMD_WRITE` emits `io_Data`/`io_Length` bytes through the task's `g_print` hook (shell window + serial) and sets `io_Actual`. `CMD_READ` completes inline when cooked input is buffered in the device's 64-byte input ring; otherwise the request pends and the event pump feeds typed characters via `ConDev_FeedChar()` (copy, not steal — GUI `WM_KeyEvent` still gets the char). Keymap commands (`CD_ASKKEYMAP`/`CD_SETKEYMAP`/`CD_ASKDEFAULTKEYMAP`/`CD_SETDEFAULTKEYMAP`) report the built-in map. `AbortIO` pulls a pending read; `Close`/`CMD_RESET` drop the task's pending reads.

## keyboard.device (`kernel/exec/keyboard_device.c`)

AmigaOS keyboard input device — real BeginIO since UAOS-240. `KBD_READEVENT` fills a guest `InputEvent` (`ie_Class = IECLASS_RAWKEY`, `ie_Code` = raw key with the `0x80` release bit, `ie_Qualifier` from `PS2Kbd_IEQualifier`, timestamped) and pends when no raw key is buffered; the event pump taps each drained `PS2Kbd_GetRawKey` transition through `KbdDev_OnRawKey()` so pending guests get a copy without stealing the IDCMP delivery. `KBD_READMATRIX` writes the qualifier byte; reset-handler commands complete as no-ops; `AbortIO` pulls a pending event read.

## timer.device (`kernel/exec/timer_device.c`)

AmigaOS timing device.

| Function | Status | Notes |
|---|---|---|
| `OpenDevice` / `CloseDevice` | Implemented | Opens/closes the device unit. |
| `BeginIO` | Implemented | `TR_ADDREQUEST` queues a `TimeRequest` in a 32-entry queue (guest fields read/written via BE helpers — `tr_secs`/`tr_micro` at +32/+36); `TR_GETSYSTIME` completes inline. |
| `AbortIO` | Implemented | Dequeues a pending `TR_ADDREQUEST`; the glue replies it with `IOERR_ABORTED`. |
| `GetSysTime` | Implemented | Returns the current time using the NTP/RTC epoch. |
| `EClockUpdate` / `ReadEClock` | Implemented | E-clock counter in microseconds (free-running, advanced by elapsed PIT ticks). |
| `AddTime` / `SubTime` / `CmpTime` | Implemented | `timeval` arithmetic. |

When a queued timer request expires (from `timer_ProcessTicks`, ~100 Hz), the entry's recorded requester RAM window gets the `UAOS_Emu_IOReply` treatment: `IOF_DONE`, node onto `mn_ReplyPort`, `Signal(mp_SigTask, 1<<mp_SigBit)`.

## ROM Module Registration

All of the above libraries and devices are registered at boot by `kernel/exec/rom_modules.c` via `UAOS_ROM_RegisterAll()`. The ROM module table supports up to 64 modules and maps names to version, base, and native function tables. The registered set also includes `exec.library`, `dos.library`, `graphics.library`, `intuition.library`, `bsdsocket.library`, `workbench.library`, `mathieeesingbas.library`, and `mathtrans.library`.

### Guest binding (UAOS-238)

Every registered module carries an `lvo_map` (`UaosRomLvo` entries, bound via `UAOS_ROM_BindLvoMap`) translating guest LVOs into 1-based `native_funcs` indices — the tables are arbitrarily ordered per module, so the map lives next to the func table it describes. Modules whose `native_funcs[]` is already indexed by `abs(LVO)/6` (graphics) instead call `UAOS_ROM_MarkSlotIndexed`.

Guest `OpenLibrary` resolves names through `UAOS_ROM_Find()` before falling back to the loadable-library and fake-base paths. A registered module gets a *generated* library base in the per-task arena at `0x007F0000–0x00800000`: a stub block (`ILLEGAL` catch-alls for the full vector area plus per-vector stubs for each `lvo_map` entry), a Library node carrying the registered `lib_Version`, and the name/id string. The version argument is honoured — requesting a newer version than `module.version` returns `NULL`. Generated bases dispatch through `emu_rom_call()` (`LIB_ROM`), which marshals D0–D7/A0–A7 into `M68kCPUState` and invokes `UAOS_ROM_NativeFunc()`. `OpenDevice` binds ROM-registered devices (`timer.device`, `console.device`, `keyboard.device`) the same way, with `audio.device` keeping its dedicated base.

Unmapped LVOs on a generated base fail predictably: the catch-all stub returns 0, except `-6` (returns the base, matching `Open` semantics) and `locale.library` `-72` (GetCatalogStr — returns the caller's default string). Unknown names still get the fake base whose vectors are no-op 0s — *except* the declined list (`emu_declined_names` in `emulation/uaos_m68k_glue.c`, UAOS-249): `amigaguide.library`, `powerpacker.library`, `lh.library`, `rexxsyslib.library` return `NULL` from `OpenLibrary`, and `serial.device` fails `OpenDevice` with `IOERR_OPENFAIL`, so apps probing for optional modules disable the feature cleanly instead of driving a working-looking stub. Matching is case-insensitive on the basename, so `libs:` paths decline too; loadable `.library` blobs in `LIBS:` still resolve first, so dropping a real implementation into `LIBS:` re-enables the feature.

Two footguns the generated path exposes: guest memory is big-endian, so module functions that dereference guest structs must read/write through byte-wise BE helpers (see `util_r32`/`util_w32`, `timer_r32`/`timer_w32` — `NextTagItem`, `GetTagData`, `AddTime`/`SubTime`/`CmpTime`, `GetSysTime`, `ReadEClock` were all converted); and functions must never leak host pointers into guest-visible registers or memory.

Guest-verified by `SYS:Demos/UtilTest` (version gate, `SMult32`/`UMult64`, tag iteration, `mathieeesingbas` `IEEESPAdd`, `timer.device` `OpenDevice`+`AddTime`, unknown-library no-op).
