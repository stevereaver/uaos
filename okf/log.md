# OKF Change Log

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
