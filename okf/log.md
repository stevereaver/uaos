# OKF Change Log

## 2026-09-24

* **Updated**: `documentation/Dos Manual.pdf` — regenerated from `Dos_Manual.md` via `pandoc --pdf-engine=pdflatex --toc --toc-depth=2 -V geometry:margin=1in`; was stale since 2026-06-14 and now includes `runback`, `resload`, and `LAB`/`SKIP` docs. (UAOS-37)
* **Updated**: `documentation/manual.pdf` — recompiled from `manual.tex` with two `pdflatex` passes; was stale since 2026-06-20 and still contained the AROS references removed in 83e97a2. (UAOS-37)
* **Created**: `okf/documentation/` category — concept files for both PDF manuals with their regeneration commands. (UAOS-37)

## 2026-09-24

* **Fixed**: `makedir` (and potentially other userspace commands) failing with "Failed to create directory." — userspace binaries were compiled *without* `-mno-red-zone`, so GCC placed `makedir`'s 256-byte `path` buffer straddling the SysV red zone below `%rsp`; the `INT 0x80` entry pushes ~160 bytes (CPU frame + saved GPRs) onto the user stack (ring-0 tasks, no privilege stack switch), clobbering `path[0..119]` → `sys_mkdir` received an empty string → `VFS_MkDir("RAM:")` returned -1. New Drawer worked because it calls `VFS_MkDir` directly in the kernel (kernel is already `-mno-red-zone`). Root cause found by booting the ISO headless in QEMU, driving the shell via monitor `sendkey`, and tracing via serial `kprint`. (UAOS-35)
* **Updated**: `scripts/build_iso.sh` — added `-mno-red-zone` to all userspace compile commands: `uaos_start.o` (steps 2e + 2ga fallback), `system/userspace/*.c`, `guide.c`, and `system/gnusrc/*.c`. (UAOS-35)
* **Updated**: `okf/build_system.md` and `okf/concepts/userspace_syscalls.md` — documented the mandatory `-mno-red-zone` userspace ABI requirement. (UAOS-35)

## 2026-09-24

* **Updated**: `kernel/display/wm.c` — reduced the zoom and depth/layer glyphs from roughly 15px to 11×7 and re-centered them in the 18×18 square cells for visible padding. (UAOS-34)
* **Investigated**: `DH0:` appears because pending boot code now auto-mounts formatted partitions under device display names, and Workbench intentionally renders every VFS mount as a desktop disk; no mount behavior was changed. (UAOS-34)
* **Updated**: `okf/kernel/display/index.md` — documented the padded title-bar glyph geometry. (UAOS-34)

## 2026-09-24

* **Updated**: `kernel/display/wm.c` and `kernel/exec/intuition_lib.c` — added the zoom gadget's missing left edge; moved activation to mouse release over the armed gadget; made it toggle full-screen/restore for every window; and synchronized all guest window geometry fields after toggling. (UAOS-33)
* **Updated**: `okf/kernel/display/index.md` and `okf/kernel/exec/intuition_library.md` — documented zoom toggle and geometry synchronization behavior. (UAOS-33)

## 2026-09-24

* **Updated**: `kernel/display/wm.c` / `wm.h` and `kernel/exec/intuition_lib.c` — enlarged title bars to 20px, switched titles to the full 8×16 font, made system-gadget cells square, added inset pressed rendering, delayed close/depth actions until release over the armed gadget, restored live resize interaction with pressed/release feedback, and synchronized guest `Window.Width`/`Height` during native resize events. (UAOS-32)
* **Updated**: `okf/kernel/display/index.md` — documented title-bar geometry and system-gadget press/release behavior. (UAOS-32)

## 2026-09-24

* **Updated**: `kernel/display/requester.c` — `g_req` now initializes `wm_handle` to -1 at declaration. It was BSS-zeroed to 0, so the `wm_handle >= 0` "already active" guard in `Requester_Confirm`/`Requester_String`/`Requester_Info` rejected every first open — no requester could ever appear (root cause of Window ▸ New Drawer doing nothing). (UAOS-31)
* **Updated**: `kernel/display/requester.c` — added `req_event` (registered via `WM_SetEventHandler` in all three open functions) so close-gadget clicks route through `Requester_Close` as Cancel instead of bypassing it via `WM_CloseWindow`, which left `g_req.wm_handle` stale and blocked all future requesters. The open guard now also checks `WM_GetDrawFn(handle) == req_draw` to recover from stale/reused slots. (UAOS-31)
* **Updated**: `kernel/display/requester.c` — `req_key` now calls `Requester_Close()` before invoking the completion callback in all four dismiss branches (Enter/Escape, string/non-string), matching `req_release`. Previously the requester still held WM focus during Enter/Escape callbacks, so `new_drawer_cb`'s `FileBrowser_GetFocusedPath()` would have returned NULL. (UAOS-31)
* **Updated**: `kernel/display/desktop.c` — New Drawer stashes the target browser path + wm handle in `g_pending_mkdir` at menu-dispatch time (same pattern as `g_pending_delete`); `new_drawer_cb` builds the path and refreshes the browser from the stash instead of re-querying focus. (UAOS-31)
* **Updated**: `okf/kernel/display/index.md` — added a Requesters section documenting the close-before-callback contract; expanded the New Drawer menu-table row. (UAOS-31)

## 2026-09-24

* **Updated**: `kernel/dos/blockdev.c` — `BlockDev_Read`/`BlockDev_Write` now serialize all block I/O with cli/sti (single in-flight VirtIO transaction); 4K-aligned static `blockdev_boot_sector` replaces stack buffers; `BlockDev_ReadVolLabel` strips only trailing spaces from FAT labels. (UAOS-28)
* **Updated**: `kernel/dos/partition.c` — 4K-aligned static `part_sector_buf` for MBR and UAOS-meta sector I/O (DMA-safe for VirtIO). (UAOS-28)
* **Updated**: `kernel/irq/virtio_blk.c` — `mfence` before device notify; `g_virtq_free_idx` reset on request timeout. (UAOS-28)
* **Updated**: `kernel/display/shell_win.c` + `kernel/exec/syscall_dispatch.c` — `Task_ClearSig(SIGF_CHILD)` before `Wait()` fixes the redirect-output race; `format` prints per-stage FAT32 error codes. (UAOS-28)
* **Updated**: `kernel/dos/ramfs.c` — shared data pool 1MB → 8MB. (UAOS-28)
* **Updated**: `kernel/shell/cmd_format.c` — per-stage FAT32 format error codes. (UAOS-28)
* **Updated**: `.devin/skills/plane-tracking/SKILL.md` — documented the two-axis Plane label taxonomy (`area:*` + `type:*`) so new cards get labeled consistently. (UAOS-30)

## 2026-09-23

* **Created**: `kernel/exec/float_math.c` / `float_math.h` — shared freestanding IEEE 754 single-precision transcendental helpers (`uaos_sinf`, `uaos_cosf`, `uaos_tanf`, `uaos_sqrtf`, `uaos_expf`, `uaos_logf`, `uaos_asinf`, `uaos_acosf`, `uaos_atanf`, `uaos_floorf`, `uaos_ceilf`, `uaos_powf`), extracted from `mathtrans_lib.c`. (UAOS-11)
* **Updated**: `kernel/exec/mathffp_lib.c` — `SPSqrt`, `SPLog`, `SPExp`, `SPSin`, `SPCos`, `SPTan`, `SPAtan`, `SPAsin`, `SPAcos` now call the shared `float_math.c` helpers instead of returning NaN/inf stubs. (UAOS-11)
* **Updated**: `kernel/exec/mathtrans_lib.c` — `my_*` implementations moved to `float_math.c`; wrappers now call `uaos_*` exports.
* **Updated**: `scripts/build_iso.sh` — added `kernel/exec/float_math.c` to the kernel source list.

## 2026-09-24

* **Created**: `kernel/shell/cmd_runback.c` — `C:runback <cmd>` queues a command line as a background job by dispatching `<cmd> &` (the shell's existing `&` job-queue path). (UAOS-17)
* **Created**: `kernel/shell/cmd_alias.c`, `cmd_unalias.c`, `cmd_path.c` — native `C:` entries that forward to the shell built-ins via `dispatch_line` (builtins still win for bare names). (UAOS-17)
* **Created**: `kernel/shell/cmd_skip.c`, `cmd_lab.c` — native `C:` entries for the `skip`/`lab` script keywords; `skip` prints a note outside scripts, `lab` is a no-op marker. (UAOS-17)
* **Created**: `kernel/shell/cmd_resload.c` — `C:resload <cmd>` forwards to `resident <cmd>` to load commands into the resident list. (UAOS-17)
* **Updated**: `kernel/shell/native_cmd.c` / `native_cmd.h` — registered `runback`, `alias`, `unalias`, `path`, `skip`, `lab`, `resload` in the native command table. (UAOS-17)
* **Updated**: `scripts/build_iso.sh` — added the seven new `cmd_*.c` sources/objects and generated `C:` stub binaries for them. (UAOS-17)
* **Updated**: `kernel/display/shell_win.c` — `help` output lists `runback` and `resload`. (UAOS-17)
* **Updated**: `documentation/Dos_Manual.md` — documented `runback`, `resload`, and the `LAB`/`SKIP` script keywords. (UAOS-17)

## 2026-09-24

* **Updated**: `kernel/display/desktop.c` — desktop icons can now be dropped onto the Trashcan to delete them. The Trashcan is a valid drop target (highlighted via `g_drop_target_idx`/`draw_drop_target_highlight`) for volume and leave-out drag sources; on release `desktop_confirm_delete()` shows the same "Delete 'name'?" requester as Icons ▸ Delete and `desktop_move_to_trash()` moves the path to `RAM:/Trash/` — both refactored out of `menu_action_icon_delete`/`delete_cb` so the menu and drag paths share one implementation via `g_pending_delete`. (UAOS-8)
* **Updated**: `okf/kernel/display/index.md` — Drag-to-Copy section renamed to Icon Drag and Drop; documents the Trashcan drop target and shared delete path. (UAOS-8)
* **Updated**: `kernel/exec/graphics_lib.c` — `BlitSurface` carries the guest `BitMap` pointer; window `RastPort`s on a screen `BitMap` get screen-relative `dx`/`dy`; planar writes track a dirty bounding box (`bm_note_dirty`) flushed per-dispatch to `UAOS_Intuition_FlushScreenBitmap`; added `render_bitmap_region_to_framebuffer` source-rect variant; `planar_fill_rect` applies `dx`/`dy`. (UAOS-22)
* **Updated**: `kernel/exec/intuition_lib.c` — every screen is backed by a planar `BitMap` (allocated via `AllocBitMap` when `SA_BitMap` is absent) plus a built `ColorMap` and minimal `ViewPort`; `Screen.RastPort` and window `RastPort`s draw into it; `UAOS_Intuition_RenderScreenBackdrop` renders the front screen `BitMap` with `SA_BackFill` first; added `UAOS_Intuition_FlushScreenBitmap` and the `intu_screen_vacate` WM hook; `AllocScreenBuffer` now allocates real planar `BitMap`s; `extract_screen_palette` seeds pens 0–3 with the Workbench palette (grey/black/white/blue) so screens without `SA_Colors` get the correct grey backdrop; `init_guest_rastport` sets `Mask=0xFF` so `apply_write_mask` does not drop writes. (UAOS-22)
* **Updated**: `kernel/exec/graphics_lib.c` — `InitRastPort` sets `Mask=0xFF` (AmigaOS default). (UAOS-22)
* **Updated**: `kernel/display/wm.c` / `wm.h` — `WM_SetVacateFn` hook invoked before a window vacates its rect on move/resize/zoom/close. (UAOS-22)
* **Updated**: `scripts/build_iso.sh` — added `float_math.o` to the kernel link list (source was already compiled but never linked). 
