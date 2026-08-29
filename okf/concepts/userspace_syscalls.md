---
type: Concept
title: Userspace and System Calls
description: Details of the Ring-0 user-space task execution model, software interrupt INT 0x80 system call dispatching, and native userspace utility execution in UAOS.
tags: [userspace, syscalls, ring0, elf64, uaos-binary]
timestamp: 2026-08-29T19:00:00Z
---

# Userspace and System Calls

UAOS implements a Ring-0 task execution model for native 64-bit ELF binaries. These tasks run with memory protection (MMU sandboxing) and execute system functions via a software interrupt system call interface.

> [!NOTE]
> X64 userspace tasks currently execute in **Ring 0** (kernel code segment `0x08`, kernel data segment `0x10`) rather than Ring 3. This works around broken ring transitions in VirtualBox NEM (Hyper-V) mode. The `INT 0x80` syscall gate retains `DPL=3` so Ring-3 execution can be re-enabled in the future by switching back to user segments (`0x1B`/`0x23`) when the VirtualBox bug is resolved. Despite running in Ring 0, userspace tasks still have MMU-sandboxed address spaces and are killed gracefully on CPU exceptions rather than halting the system.

## Execution Model

Native userspace programs are compiled as position-independent executables (PIE) and packaged in the custom `UAOS` binary format:
- **Header Magic**: Starts with `UAOS` magic bytes (`'U'`, `'A'`, `'O'`, `'S'`).
- **Binary Type**: Set to `0x0003` (`UAOS_TYPE_X64`) for x86-64 userspace executables.
- **Loading & Setup**: The ELF64 loader parses the segment headers, copies sections to allocated physical pages, maps them in the task's 4-level page tables, and initializes a kernel-mode stack.
- **Ring-0 Transition**: The scheduler builds a synthetic interrupt frame with kernel CS/SS and transitions via `iretq` targeting the ELF entry point. No TSS ring switch is needed since the tasks run in Ring 0.

## System Call Interface (`INT 0x80`)

Userspace programs request kernel operations using `INT 0x80`. 

### Syscall Return Path

The `INT 0x80` handler saves the interrupted task's GPRs and, after the C dispatcher returns, uses the same `iretq`-based restore path as the hardware interrupt stubs. This means:

- New X64 tasks are first entered by restoring the synthetic interrupt frame built by `Task_CreateX64` (kernel CS/SS, ELF entry point, and kernel stack).
- On subsequent syscalls or context switches, the handler restores the previously saved kernel stack frame and returns to the instruction following the `INT 0x80`.

There is no special-case "first-launch" branch in the syscall handler; using the shared `iretq` path prevents accidental task restart loops that would freeze the system when a userspace program calls the kernel.

### Registers
Syscalls use the following register layout for parameter passing:
- **RAX**: Syscall number (e.g., `0x01` for write, `0x07` for exit, `0x0C` for getcwd)
- **RDI**: Argument 1
- **RSI**: Argument 2
- **RDX**: Argument 3
- **Return Value**: Returned in **RAX** (negative values represent errors)

### Syscall Table
The system call dispatcher (`Syscall_Dispatch` in `kernel/exec/syscall_dispatch.c`) supports the following `INT 0x80` calls, defined in `kernel/exec/syscall_table.h` and `system/libuaos/uaos_syscall.h`:

| Syscall Number | Name | Description |
|---|---|---|
| `0x01` | `sys_write` | Writes data to file descriptor 1 (stdout) or 2 (stderr). Output is line-buffered via the task's line printer. |
| `0x02` | `sys_read` | Reads data from file descriptor 0 (stdin). |
| `0x03` | `sys_open` | Opens a file using a path relative to the task's current working directory. |
| `0x04` | `sys_close` | Closes a VFS file descriptor. |
| `0x05` | `sys_read_file` | Reads data from a VFS file descriptor opened by `sys_open`. |
| `0x06` | `sys_write_file` | Writes data to a VFS file descriptor opened by `sys_open`. |
| `0x07` | `sys_exit` | Flushes task output buffers and terminates the task. |
| `0x08` | `sys_getargs` | Copies the command-line arguments passed to the task into a user buffer. |
| `0x09` | `sys_spawn` | Spawns a new Ring-0 task executing an ELF64 binary, copying the parent's environment. |
| `0x0A` | `sys_wait` | Blocks or yields CPU execution. |
| `0x0B` | `sys_alloc` | Allocates memory pages for the calling task. |
| `0x0C` | `sys_getcwd` | Retrieves the task's current working directory (`task_cwd`). |
| `0x0D` | `sys_opendir` | Opens a directory handle for listing its contents. |
| `0x0E` | `sys_readdir` | Reads the next entry in a directory handle into a `uaos_dirent`. |
| `0x0F` | `sys_closedir` | Closes an active directory iteration handle. |
| `0x10` | `sys_stat` | Retrieves size, directory status, and attributes of a file/directory path. |

### GUI / Windowing Syscalls

Native userspace programs can also create simple graphical windows through the kernel window manager using the following syscalls:

| Syscall Number | Name | Description |
|---|---|---|
| `0x11` | `sys_gui_create_window` | Creates a WM window with title, position, and size; returns a window handle. |
| `0x12` | `sys_gui_destroy_window` | Destroys a userspace GUI window. |
| `0x13` | `sys_gui_set_scroll_info` | Sets total content size and visible viewport for scrollbars. |
| `0x14` | `sys_gui_set_scroll` | Sets the current scroll offset. |
| `0x15` | `sys_gui_draw_text` | Draws text into the window's backing buffer. |
| `0x16` | `sys_gui_draw_rect` | Draws a filled rectangle into the window's backing buffer. |
| `0x17` | `sys_gui_present` | Blits the backing buffer to the screen. |
| `0x18` | `sys_gui_get_event` | Reads the next input event (mouse, key) from the window's event ring. |

### Filesystem Metadata & Mutation Syscalls (Phase 7)

Added to support the migration of DOS commands from kernel-resident C: stubs to on-disk userspace binaries:

| Syscall Number | Name | Description |
|---|---|---|
| `0x20` | `sys_mkdir` | Create a directory at a path relative to the task's cwd. |
| `0x21` | `sys_delete` | Delete a file or directory. |
| `0x22` | `sys_rename` | Rename or move a file/directory. |
| `0x23` | `sys_setprotection` | Set AmigaDOS protection bits (`FIBF_*`) on a file. |
| `0x24` | `sys_getprotection` | Read AmigaDOS protection bits from a file. |
| `0x25` | `sys_getcomment` | Read a file's comment string. |
| `0x26` | `sys_setcomment` | Set a file's comment string. |
| `0x27` | `sys_getvolumeinfo` | Query total/used bytes for a volume path. |
| `0x28` | `sys_readkey` | Read a single keypress (yields until input available). |
| `0x29` | `sys_getattrs` | Retrieve file attributes (size, type, timestamps). |
| `0x2A` | `sys_setattrs` | Set file attributes. |
| `0x2B` | `sys_getmountcount` | Return the number of mounted VFS volumes. |
| `0x2C` | `sys_getmountname` | Get the name of a mounted volume by index. |
| `0x2D` | `sys_meminfo` | Fill a `uaos_meminfo` struct with a point-in-time snapshot of x64 heap, M68k guest RAM, and scheduler task counts. Backed by the kernel `Mem_GetInfo()` helper. |

### Extended GUI Drawing Syscalls

Back the userspace widget toolkit (`uaos_gui.h`); implemented in `kernel/display/user_window.c`:

| Syscall Number | Name | Description |
|---|---|---|
| `0x30` | `sys_gui_draw_line` | Bresenham line drawing. |
| `0x31` | `sys_gui_fill_rect` | Filled rectangle. |
| `0x32` | `sys_gui_draw_3dborder` | Raised/recessed 3D bevel with auto-shading. |
| `0x33` | `sys_gui_draw_pixel` | Single pixel. |
| `0x34` | `sys_gui_draw_text_bg` | Text with foreground and background colours. |
| `0x35` | `sys_gui_get_winsize` | Query window client area dimensions. |
| `0x36` | `sys_gui_set_title` | Update window title bar text. |
| `0x37` | `sys_gui_draw_ellipse` | Ellipse outline (midpoint algorithm). |

### Scheduler Yield

| Syscall Number | Name | Description |
|---|---|---|
| `0xFF` | `sys_schedule` | Yield to the scheduler (cooperative yield). |

## Per-Task Environment & IPC

- **Current Working Directory (`task_cwd`)**: Copy-on-write directory string stored inside the `UaosTask` struct. Used by the VFS to resolve relative paths.
- **Output Redirection**: Each userspace task registers a custom line printer (`native_print_fn`) and context (`native_print_ctx`). Syscall output is buffered in `task_out` and printed line-by-line or on exit.
- **Parent/Child Signaling**: A parent task tracks child execution. When a child task exits, it signals its parent with `SIGF_CHILD` (used by the CLI shell to wait for foreground commands).

## Freestanding Library Support

To run without linking a host standard C library:
- `uaos_start.c`: The startup object (`uaos_start.o`) providing the entry point (`_start`) that wraps parameter parsing and invokes userspace `main`.
- `uaos_libc.h`: A header-only minimal libc providing freestanding implementations of `strlen`, `strcmp`, `strcpy`, `strncpy`, `strlcat`, `memcpy`, `memset`, `memcmp`, `strchr`, `isdigit`, `isprint`, `isspace`, `toupper`, and `tolower`.
- `uaos_syscall.h`: Syscall inline wrappers (`uaos_open`, `uaos_getcwd`, `uaos_readdir`, `uaos_gui_create_window`, etc.) mapping standard actions to `INT 0x80`.

## Userspace Utilities

Built from source files in `system/userspace/` and packaged into `SYS_ROOT/C/`:

1. `pwd`: Asks the kernel for the task's cwd and prints it.
2. `find`: Walks directory trees recursively using `opendir`/`readdir` with optional `-name` and `-type` filtering.
3. `file`: Opens a file, inspects magic numbers, and reports if it is ASCII, a native x86-64 binary, an Amiga Hunk binary, etc.
4. `strings`: Scans binary or text files for printable character sequences and outputs them; supports `-n` for minimum length.
5. `Guide`: AmigaGuide viewer that opens a GUI window, parses node links, and navigates between guide nodes using the GUI syscall interface.
6. `hello`: A simple test utility printing a hello message to verify userspace execution functionality.

### Migrated DOS Commands (Phase 7)

The following 16 DOS commands were migrated from kernel-resident native C: stubs to on-disk x86-64 ELF64 userspace binaries, using the VFS metadata/mutation syscalls (0x20–0x2C) and the shared helpers in `system/libuaos/`:

`echo`, `type`, `dir`, `list`, `makedir`, `delete`, `rename`, `copy`, `protect`, `attr`, `grep`, `sort`, `join`, `search`, `filenote`, `more`.

`avail` was likewise migrated off its hardcoded native stub to a userspace binary that queries real memory statistics via `SYSCALL_MEMINFO` (0x2D). See [System Layout](/system/index.md) for the full list with templates, and [Shell](/kernel/shell/index.md) for the command reference.
