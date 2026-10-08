---
type: Kernel Subsystem
title: DOS Library & Handler System
description: The Amiga-inspired Disk Operating System and packet-based handler architecture.
resource: /kernel/dos/
tags: [dos, vfs, filesystem, handlers]
timestamp: 2026-10-06T11:57:25Z
---

# DOS Library & Handler System

The DOS library manages file system access and device I/O through a packet-based asynchronous architecture. The AmigaOS-compatible `dos.library` implementation lives in `kernel/exec/dos_lib.c`; the lower-level VFS, handlers, and filesystem drivers live in `kernel/dos/`.

## Handler Architecture

In UAOS, filesystems and devices are managed by "Handlers". A handler is an object that listens on a `MsgPort` for `DosPacket` messages.

### DosPacket Actions

Common actions include:
- `ACTION_FINDINPUT`: Open a file for reading.
- `ACTION_FINDOUTPUT`: Open a file for writing (create-or-truncate — `VFS_Open` sends it when `VFS_TRUNC` is set).
- `ACTION_READ`: Read data.
- `ACTION_WRITE`: Write data.
- `ACTION_EXAMINE`: Read a directory entry.
- `ACTION_EXAMINE_NEXT`: Read the next directory entry.
- `ACTION_FINDUPDATE`: Open an existing file for read/write without truncating (`VFS_Open` sends it for `VFS_WRITE`/`VFS_CREATE` without `VFS_TRUNC`; handlers create the file when missing). This is what makes the shell's `>>` append redirect work — before UAOS-89 `VFS_CREATE` alone triggered `FINDOUTPUT`, silently truncating the append target.
- `ACTION_DELETE`: Delete a file or directory.
- `ACTION_CREATE_DIR`: Create a directory. Handler-backed filesystems return a lock handle on success (not `DOSTRUE`); `VFS_MkDir` releases that lock before reporting success.

## Virtual File System (VFS)

The VFS layer (`kernel/dos/vfs.c`) provides a unified interface for multiple filesystems and the AmigaDOS assign system. Supported filesystems:

- **RAMFS**: An in-memory filesystem mounted at boot with `T:`, `ENV:`, `ENVARC:`, `Clips:`, `S:`, and `Trashcan` + `Trashcan.info` (RAM: is "formatted" at init, so it gets a Trashcan like any freshly formatted Amiga volume; see `VFS_CreateTrashcan` below).
- **FAT32**: Read/write support for physical disk partitions (`kernel/dos/fat32.c`, `fat_handler.c`). Nested file/directory creation resolves the parent path to the found directory entry's own cluster (not the cluster containing that entry) and verifies the parent has the directory attribute. `VFS_ReadDir` uses the FAT handler's direct enumeration API (`FatHandler_ReadDir`) rather than the generic packet EXAMINE sequence. `FAT32_Mount` validates the BPB is genuinely FAT32 — OEM signature not "EXFAT", `root_ent_cnt==0`, `fat_sz16==0`, `fat_sz32>=1`, `root_clus>=2`, `bytes_per_sec` in 512..4096, `sec_per_clus` a power of two ≤128, `num_fats>0`, `total_sectors>0` — so FAT12/16, exFAT, and malformed media are rejected at mount instead of mounting and failing on every operation.
  - **VFAT long file names** (UAOS-254): lookups (`fat32_find_in_dir`) assemble each entry's LFN chain — fragments fed in disk order into a `Fat32Lfn` accumulator, sequence- and checksum-validated against the 8.3 alias (orphaned/mismatched chains are ignored), UCS-2 → Latin-1 with non-Latin-1 code points read as `_` — and match the requested component case-insensitively (Latin-1 fold) against the long name **or** the 8.3 alias, so `OctaMED.guide`, `octamed.GUIDE` and `OCTAME~1.GUI` all open the same file. 8.3-only entries honour the NT lowercase flags (`nt_res` 0x08/0x10) so Linux/Windows `readme.txt` displays lowercase. `FAT32_ReadDir(dir, name, name_max, …)` returns the long name, falling back to the alias when it doesn't fit `name_max` (EXAMINE paths pass the 108-byte `fib_FileName`; `FatHandler_ReadDir` passes `VfsDirEnt.name`, 32 bytes). Create/mkdir/rename go through `fat32_add_entry`: `fat32_make_sfn` keeps plain uniformly-cased 8.3 names as a single entry (+NT flags), everything else gets a Windows-style basis name with the first free `~N` tail and an LFN chain written into a run of consecutive free slots (`fat32_find_free_run`, may cross clusters, extends the directory). Names containing `"*/:<>?\|` or control chars, or that are all dots/spaces, are rejected. Delete removes the whole chain (`fat32_remove_entry`). `ReadDir` iterates through a private `g_dir_buf` tagged by cluster and invalidated by every write (`fat32_bwrite`) and every `FAT32_Open`, so ExNext loops survive interleaved I/O (previously it trusted the shared `g_cluster_buf` between calls).
  - **Rename** (UAOS-29, reworked UAOS-254): `FAT32_Rename` handles `ACTION_RENAME_OBJECT` for same-directory renames. It snapshots the old 8.3 entry + LFN chain, removes them, and re-creates the entry under the new name (new alias/LFN as needed) carrying cluster, attributes, size, and timestamps; on failure the raw old entries are written back. The destination may only resolve to the source itself (case-only renames). Open `Fat32File` handles pointing at the old slot are retargeted to the new one so `FAT32_Close` can't write size/cluster into a reused slot. Cross-directory moves return an error (callers do copy+delete).
  - **Truncate** (UAOS-254): `FAT32_CreateFile` on an existing file patches cluster/size/timestamps in place instead of rebuilding the entry, keeping the alias bytes (and so the LFN checksum binding) intact.
  - **Spec conformance** (UAOS-254): new directories' `..` holds cluster 0 when the parent is the root (fsck.fat flagged the old root-cluster value); `.`/`..` get timestamps. The first FAT allocate/free per mount sets the FSINFO free-count and next-free hints to `0xFFFFFFFF` (unknown) rather than leaving the formatter's now-wrong count. Verified with `fsck.fat -n` (rc 0) and mtools `mdir`/`mtype` on volumes written by UAOS.
  - **Parent tracking** (UAOS-29): `Fat32File.parent_cluster` records the cluster of the directory holding each opened/created entry. The handler resolves `ACTION_PARENT` and `ACTION_PARENT_FH` by deriving the parent path (`fat_parent_path`) and returning a fresh lock — `Parent()`/`ParentDir()` no longer return a null lock on FAT volumes; the volume root still returns 0 per AmigaDOS semantics.
  - **Timestamps** (UAOS-29): `fat32_stamp_entry` writes FAT create/write/access stamps on file and directory creation and refreshes the write stamp on close. `FAT32_ReadDir` returns each entry's packed `wrt_time`/`wrt_date`; `FatHandler_ReadDir` converts them to Unix epoch for `VfsDirEnt.mtime` (exposed to userspace via `sys_readdir`/`sys_stat` → `VFS_GetMtime`, so `dir`/`list DATES` show real dates), and `ACTION_EXAMINE_*` fills `fib_Date` via `fat32_fatstamp_to_ds` (Amiga DateStamp convention, `ds_Days = unix_days + 2922`). `FAT32_SetDate` backs `ACTION_SET_DATE` — the packet is no longer a no-op, though nothing in-kernel dispatches it yet.
  - **Format geometry**: `FAT32_Format` iterates the `fat_sz` ↔ cluster-count dependency to convergence (up to 16 passes) so the FAT always covers every data cluster — a fixed two-pass estimate could leave tail clusters with no FAT entries, which then scan as permanently "used". FATs are zeroed in ≤128-sector (64 KiB) batches sized to the VirtIO DMA buffer / `g_cluster_buf`.
  - **Format Trashcan** (UAOS-36): On real AmigaOS the Trashcan is a filesystem object created by Format, not a global desktop icon. `VFS_CreateTrashcan(vol)` (after `FAT32_Format` + remount) creates `VOL:Trashcan` and generates a `WB_GARBAGE` `Trashcan.info` via `Icon_Save`. Every format entry point (`format` builtin, `C:format`, Format window) creates it by default; the `NOICON` option (or the unchecked Trashcan checkbox) skips it.
  - **Mount clamp**: `fs->total_clusters` is clamped to `fat_sz32 * (bytes_per_sec/4) - 2` at mount, so volumes written by a non-converged formatter can't report phantom used tail clusters or walk past the FAT end.
  - **Volume statistics**: `FAT32_GetVolumeStats` caches the free-cluster count in `fs->free_clusters` (`-1` = unknown). The one-time FAT scan reads in `FAT32_MAX_CLUSTER_SECS` chunks (one sector per read made `info`/`dir` look hung on multi-GB volumes and could trip VirtIO timeouts); a partial/erroring scan is never cached. `fat32_alloc_cluster`/`fat32_free_chain` keep the cache exact on allocate/free, and all cluster-chain walks are bounded by `total_clusters` so a corrupt/cyclic chain can't loop forever.
  - **Handler open**: the FAT handler refuses `FINDINPUT`/`FINDUPDATE` on directories (`ERROR_OBJECT_WRONG_TYPE`), so `open("DH0:dir")` can't create a handle that later stats as a 0-byte file.
- **CrossDOS (FAT12/16)**: Read-only support for PC-format floppy disks and small partitions (`kernel/dos/crossdos_handler.c`). Probes boot sector to distinguish FAT12 from FAT16, reads root directory and file cluster chains. Mounted via `crossdos` shell command.
- **Amiga OFS/FFS**: Read/write support for the classic Amiga filesystems (`kernel/dos/ffs.c` + `ffs_handler.c`) — `DOS\0` (OFS), `DOS\1` (FFS), and the INTL/dircache variants `DOS\2`–`DOS\7` which share the same structures. The driver validates the bootblock `DOS\x` signature and (when present) its 2-block longword checksum — WinUAE hardfiles ship it unset, which is "not present", not corrupt — locates the root block near `num_blocks/2`, and mounts under the BSTR volume name in root-block longword 108. Name lookup uses the real AmigaDOS hash (`h=len; h=(h*13+upper(c))&0x7FF` per char, then `h % ht_size`, ht_size=72 for 512-byte blocks) with same-hash chains at longword 124. File data pointers are anchored at `lw(5+ht_size-i)` in both file headers and `T.LIST` extension blocks (extension link at lw126, parent at lw125); FFS data blocks are raw 512-byte payload, OFS blocks carry a 24-byte header and 488-byte payload. The bitmap (root lw78 flag, 25 page keys at lw79, `bm_ext` index chain at lw104) is advisory — bit=1 means free and WinUAE images do not mark every used block.
  **Write path** (UAOS-89): `FFS_AllocBlock`/`FFS_FreeBlock` maintain the bitmap with per-page checksums — blocks 0-1 are never allocatable (the advisory bitmap may leave them marked free; the first alloc claims them, which is also why a naive `key==0` failure check must never see them). `FFS_WriteAt` grows files block-by-block through `ffs_append_ptr`/`ffs_file_append_block`, linking `T.LIST` extension blocks once the inline 72-pointer table fills; OFS data blocks get the 24-byte header, 1-based seq numbers, forward links and checksums. `FFS_SetFileSize` extends (zeroed) and truncates (frees data blocks + trims pointer tables + drops ext chain). `FFS_CreateFile`/`FFS_CreateDir`/`FFS_Delete`/`FFS_Rename` maintain the parent hash chains, parent keys, names and header checksums; delete refuses non-empty dirs. `ffs_handler.c` maps the full packet set: `FINDINPUT`/`FINDUPDATE`/`FINDOUTPUT` (create-or-truncate), `READ`/`WRITE`/`SEEK`/`END`, `SET_FILE_SIZE`, `CREATE_DIR`, `DELETE_OBJECT`, `RENAME_OBJECT`, `SET_PROTECT`/`SET_COMMENT`/`SET_DATE`, plus the read-side lock/examine packets. Verified end-to-end in QEMU against `tests/test-ffs.hdf` attached writable (create/write/append/mkdir/rename/delete, a 54 KB file through `T.LIST` extensions, persistence across reboot, `fsck` clean). OFS (`DOS\0`) writes are code-reviewed but not live-tested.
- **PFS3**: Professional File System 3 support (`kernel/dos/pfs3.c`).
- **EXT4**: Read-only EXT4 support (`kernel/dos/ext4.c`).
- **ISO9660**: CD-ROM read support (`kernel/dos/iso9660.c`).

### Filesystem Change Counter

`g_vfs_change_seq` in `vfs.c` is bumped by `VFS_NoteChange()` whenever directory-visible content changes: the `VFS_*` functions bump it on their direct RAMFS paths (`VFS_MkDir`/`VFS_Delete`/`VFS_Rename`/`VFS_RenameVol`/`VFS_Open` create+truncate), and `DoPkt()`/`Handler_CheckReplies()` in `handler.c` bump it for mutating packet actions (`CREATE_DIR`, `DELETE_OBJECT`, `RENAME_OBJECT`, `RENAME_DISK`, `FINDOUTPUT`, `FINDUPDATE`, `WRITE`, `SET_FILE_SIZE`) — the DoPkt hook also catches guest M68k `dos.library` calls, which dispatch packets straight to handler ports and bypass the VFS API. UI code (the file browser) snapshots `VFS_ChangeSeq()` after enumerating and re-reads the directory when it differs.

`VfsDirEnt` carries an `mtime` field (Unix epoch seconds): the RAMFS branch of `VFS_ReadDir` fills it from `RamFsNode.mtime`, the generic EXAMINE path converts `fib_Date` (same `ds_Days - 2922` convention as `locale_lib.c`), and `FatHandler_ReadDir` converts each FAT entry's packed write timestamp via `FAT32_FatstampToUnix` (UAOS-29). `VFS_GetMtime(path)` exposes the same value for single paths (RAMFS node `mtime`, or a LOCATE+EXAMINE `fib_Date` conversion on handler volumes) — `sys_readdir`/`sys_stat` populate `uaos_dirent.mtime`/`UaosUserStat.mtime` from both.

### Assigns

`g_assigns[MAX_ASSIGNS]` (64 slots, ~line 1177 of `vfs.c`) holds the assign table; each entry allows up to `ASSIGN_MAX_TARGETS` (8) multi-assign targets. `resolve_assign_path()` fully expands *chained* assigns (e.g. `ACElib:` → `ACE:lib` → `SYS:ACE/lib` → `Workbench:ACE/lib`) with a depth cap of 8 — the same helper resolves non-DEFER targets inside `VFS_AddAssign`, so assigns may be rooted at other assigns, and each expanded target in the multi-assign loops (`VFS_Open`/`VFS_OpenDir`/`VFS_ResolveDir`) is re-resolved for the same reason. Capacity drops are not silent: `VFS_AddAssign` emits a `kprint("[VFS] WARN: ...")` on a full table or max-targets so `Assign >NIL:` redirects in Startup-Sequence can't hide them.

**Multi-assign write fallback** (UAOS-248): `VFS_Open`'s multi-target path searches read-only *and* read-write targets in order for opens (phase 1), then for creates/updates skips `read_only` targets and lands the file on the first writable one (phase 2) — this is what lets `S:` (`Workbench:S` on the read-only ISO proxy + a writable `RAM:S` second target) serve reads from the ISO while writes fall through to RAM:. `VfsFile.resolved_path` records the winning canonical path at every `VFS_Open` success return; `dos_Open`/`dos_OpenFromLock` store it in the `HandleTable` entry instead of the caller's unresolved name, so follow-up packet ops (`DupLockFromFH`, `ParentOfFH`, etc.) hit the target that actually holds the object rather than re-resolving to the first assign dir.

**Guest dos.library path ops** (UAOS-248): `dos_path_pkt()` in `dos_lib.c` gives `DeleteFile`/`SetProtection`/`SetComment`/`CreateDir` the same per-target search `dos_Lock` uses (`dos_expand_target` expands `VOL:rest` through each assign target), so writes through multi-assigns find the writable target. `dos_Rename` expands old and new names through the *same* target index per attempt, keeping the pair on one volume (AmigaDOS renames are same-volume). `dos_Lock` uses the shared helper too.

**RamFS lock iterator** (UAOS-248): `HandleTable_AllocLock` starts `u.lock.iter_next` NULL; ram_handler now primes it to `first_child` at every lock creation (`prime_dir_iter` on `LOCATE_OBJECT`, `PARENT`, `PARENT_FH`, `COPY_DIR`, `CURRENT_VOLUME`) — previously the first `EXAMINE_NEXT`/`EXAMINE_ALL` on a fresh lock always failed `ERROR_NO_MORE_ENTRIES` and only the failure-side reset made the second call work, which broke `ExAll`. `ACTION_PARENT`/`PARENT_FH`/`COPY_DIR`/`CURRENT_VOLUME` also store a real path on the lock entry (parent derived by `parent_path_of` stripping the last component) instead of `""`, so `dos_lock_port`/`SameLock`/`ParentDir` on derived locks resolve correctly. `dos_SameLock` compares `u.lock.node` identity first, falling back to case-folded canonical-path compare.

**LVO gap-fill** (UAOS-248): the dos.library LVO table in `uaos_m68k_glue.c` now covers `Info`/`DiskInfo`/`IsFileSystem`/`Inhibit`/`DeviceProc`/`GetDeviceProc`/`FreeDeviceProc`, `ExamineFH`/`ParentOfFH`/`DupLockFromFH`/`OpenFromLock`/`SameLock`, `SetFileSize`, `SetComment`, `ExAll`/`ExAllEnd`, `AssignLock`/`Late`/`Path`/`Add`, `CurrentDir`, `SetProgramDir`/`GetProgramDir`, and the `LockDosList` family. `dos_fib_to_guest` marshals the native `FileInfoBlock` to guest big-endian layout for `Examine`/`ExamineNext`/`ExamineFH`; `dos_ExAll` serialises `ExAllData` entries (BSTR names/comments carved from the guest free-list heap, released by `ExAllEnd`). `ID_*`/`DLT_*` constants in `amiga_dos_types.h` were corrected to real AmigaDOS values (`ID_VALIDATED`=81 etc.) so guests don't misread disk state. FAT32 gained `FAT32_SetFileSize` (real truncate via cluster free-chain) and `FAT32_SetProtection`; ram_handler's `SET_FILE_SIZE` does a real realloc-shrink/grow. Regression coverage is the guest-side `SYS:Demos/DOSTest` (`system/Demos/src/DOSTest.s`), which exercises all of the above and prints per-test PASS/FAIL. Verified: OctaMED V5 opens `S:` config files, loads `OCTAMED:SONGS/*.med` modules and `OCTAMED:INSTRUMENTS/*.instr` through dos.library, and logs zero `[dos] unimpl` hits.

### RAMFS Data Pool

RAMFS uses a shared 8 MB data pool (`g_pool` in `kernel/dos/ramfs.c`) for file content across all volumes. The pool is managed by an address-ordered free-list allocator with coalescing: every chunk (live or free) carries a 16-byte `PoolChunk` header (`size` + `next`), `RamFS_AllocPool` is first-fit on the free list and otherwise carves fresh chunks below the `g_pool_top` high-water mark, and `RamFS_FreePool` inserts in address order and merges physically adjacent free chunks. `g_pool_used` tracks committed chunk bytes (header included). `RamFS_Delete` and the `RamFS_Rename` overwrite path return `node->data` to the pool, and `RamFS_Write` frees the old buffer when regrowing.

`VFS_Write` grows node storage instead of the old first-write fixed 4 KB block: when a write would overflow `node->alloc` it allocates `max(alloc*2, VFS_BLOCK_SZ=4 KB, end)` (capped at `RAMFS_MAX_FILESIZE`), copies the existing data, frees the old chunk, and continues. If `RamFS_AllocPool` fails the write degrades to a *short write* — only the bytes that still fit are written and the partial count is returned so callers can detect the failure (`cmd_copy_file`, the shell-window `copy`, and the desktop copy helpers all treat a short write as an error and delete the partial destination).

`RamFS_Read`'s ext-backed proxy path caches the last filesystem block read (`g_sec_buf`, keyed on bdev + block size + device sector): sequential 256 B reads that land in the same 2048-byte ISO block now cost one `BlockDev_Read` per block instead of one per call (~8x I/O amplification removed).

Volume stats are per-volume: `RamFS_GetVolumeStats` walks the volume's own node tree summing each file's contribution — `node->alloc` for pool-backed files, `node->size` for ext_bdev proxy files (their content consumes no pool but still belongs to the volume; without it the all-proxy `Workbench:` sysroot mount reported `used = 0`, UAOS-222) — and reports `total = used + pool_free` where `pool_free = pool_size - g_pool_used`, so every volume's `free` is the shared pool's writable headroom while `used` is its own. **Read-only volumes** (`RamFsVol.read_only`, UAOS-268) cannot grow, so they report `total = used` (free = 0).

A volume is marked read-only with `RamFS_SetReadOnly` — used by `ISO9660_MountCD` for proxy mounts (file data lives on the read-only ISO image, so the volume is a CD). Once set, every mutation entry point rejects the volume: `RamFS_MkDir`/`Create`/`Delete`/`Rename`/`RenameVol` at the vol level, and `RamFS_Write`/`SetAttrs`/`SetProtection` at the node level via a `vol_of_node` root-walk (exposed as `RamFS_NodeReadOnly`). The VFS layer adds matching guards because it mutates nodes directly: `VFS_Open` denies `VFS_WRITE`/`VFS_TRUNC` intents on RO volumes, `VFS_Write` early-outs on `RamFS_NodeReadOnly`, and `VFS_SetComment` checks `vol->read_only`. `ram_handler`'s `ACTION_DISK_INFO` reports `ID_WRITE_PROTECTED`, and `VFS_GetVolumeInfo`'s optional `read_only` out-param surfaces the state so `info` can print `Read-Only`.

`DEFAULT_PROTECTION` in `amiga_dos_types.h` is `FIBF_OTR_WRITE|FIBF_OTR_EXECUTE|FIBF_OTR_DELETE|FIBF_GRP_WRITE|FIBF_GRP_EXECUTE|FIBF_GRP_DELETE` (0xEE00): all R/W/E/D bits use inverted logic (bit set = denied), so owner rwed and group/other read stay clear (allowed) while group/other write+execute+delete are denied. The previous `0xFFE0|GRP_READ|OTR_READ` encoding (= 0xFFE0) set `FIBF_DELETE` on every new node, making fresh files undeletable/unrenamable.

## Block Devices and Partitioning

- **Block device layer (`blockdev.c`)**: Unified interface for storage devices. `BlockDev_CheckFormatted` recognises FAT-style boot sectors and Amiga `DOS\x` bootblocks.
- **Partition table (`partition.c`)**: MBR parsing and partition registration, plus Amiga RDB support — `RDSK` is scanned in the first 16 blocks (longword-checksummed), then the `PART` chain (name BSTR at offset 36, environment vector at offset 128 with `de_TableSize`/`de_SizeBlock`/`de_SecOrg`/`de_Surfaces`/`de_Sectors`/`de_LowCyl`/`de_HighCyl` giving the true partition extent) and `FSHD` filesystem-header chain are walked. `PartitionTable.rdb_block` records the RDB location when one is found.
- **Boot auto-mount**: `boot_automount_partitions()` in `uaos_kernel_main.c` runs for **every** registered virtio-blk device (and on the virtio-scsi path). Per disk it tries MBR partitions first, then an RDB `PART` chain, then probes the whole disk as a bare filesystem (FAT32 → OFS/FFS) when no table exists — WinUAE `.hdf` hardfiles mount this way under their Amiga volume name. MBR partitions mount under their display name (DH0:, DH1:) without requiring the UAOS-meta `automount` flag. Formatting a mounted partition calls `VFS_RemountPartition()` under that same display name to refresh the handler's cached FAT32 geometry and re-read the volume label.
- **Volume labels**: each `MountEntry` carries `vol_name` (the unit name, e.g. `DH0`) plus `vol_label` read from the filesystem at mount (`FAT32_VolumeLabel`, from the BPB; "NO NAME"/empty counts as none). Both names resolve as path prefixes — `cd wb:` and `cd DH0:` reach the same disk — with unit names taking precedence so a label can't shadow a real mount or assign. `VFS_GetMountName` returns the label when present, so the desktop icon and `info`'s "Volumes available" show the volume name (e.g. `WB:`), matching AmigaDOS.
- **Mount backing device**: `MountEntry` also carries `bdev` — the `BlockDev *` the filesystem is mounted on (`NULL` for RAMFS and ISO9660-extract mounts). `VFS_GetMountInfo(idx, unit, label)` and `VFS_GetMountDev(idx)` expose the mount table so `info`'s "Mounted disks" can be driven from the mount table rather than from the block-device list — important because whole-disk bare-filesystem mounts (no partition table, `part_offset == 0`, e.g. a FAT32 `.hdf` or formatted whole disk) are mounted and resolvable but live on the parent device. `info` iterates mounts for its table and additionally lists formatted-but-unmounted partition devices (`part_offset != 0`, not matching any mount's `bdev`) so unmounted partitions still show with their capacity. Since UAOS-268 the unmounted rows report an honest status — `Not mounted` when `BlockDev_CheckFormatted` sees a filesystem signature, `No filesystem` otherwise — with `-` for the Used/Free/Full/Errs columns rather than fake `Read/Write` figures; mounted RO volumes show `Read-Only`.
- **IDE driver (`kernel/drivers/ide.c`)**: ATA/ATAPI PIO access for hard disks and CD-ROMs.
- **VirtIO Block (`virtio_blk.c`)**: VirtIO-compliant block device driver supporting multiple disks — every virtio-blk PCI function found is registered as `virtio0`, `virtio1`, ... (max 4) with its own page-aligned virtqueue, PCI base, and IRQ line; `BlockDev.private_data` routes I/O to the right device.

### Block I/O Serialization and DMA Buffers

`BlockDev_Read`/`BlockDev_Write` serialize all block I/O with `cli`/`sti` — the VirtIO drivers keep a single set of global request/response/data buffers and one in-flight descriptor, so concurrent I/O from different tasks (e.g. desktop polling vs. shell `format`/`makedir`) would otherwise corrupt the in-flight transaction. `blockdev.c` and `partition.c` use 4K-aligned static sector buffers (`blockdev_boot_sector`, `part_sector_buf`) for boot-sector, MBR, and UAOS-meta I/O because the VirtIO driver requires DMA-accessible buffers — stack buffers are not safe. `BlockDev_ReadVolLabel` picks the BPB label field by the fs-type string — offset 71 for `FAT32`, offset 43 for `FAT12/16` — rejects labels containing non-printable bytes (a mis-detected BPB can't leak garbage into displays), and strips only *trailing* spaces so labels with internal spaces (e.g. "MY DISK") read correctly.

### DMA Memory Pool (`kernel/dos/dma.c`)

`DMA_Alloc`/`DMA_Free` manage a static 2 MB page-aligned pool (`g_dma_pool` in .bss, identity-mapped) used for TDs/QHs, frame lists, descriptor rings, and bounce buffers. Since UAOS-173 the pool is a K&R-style first-fit free list under `irq_save`/`irq_restore` critical sections: every block carries a 16-byte `DmaBlk` header (`size` including header, `link` = next free block or `DMA_USED_MAGIC` when in use), free blocks are threaded in address order, `DMA_Free` inserts sorted and coalesces with physically adjacent neighbours. The consumed span per allocation (`align pad + size`) is rounded up to 8 so every block base — and therefore every header field and the `payload-8` back-offset a used block stores for `DMA_Free` — stays 8-aligned. `DMA_Free` validates the pointer range, the back-offset, and the used magic, rejecting double/wild frees with a WARN. `DMA_Usage` reports used/free bytes for `C:mem`'s `DMA pool:` line; the per-alloc log sits at KLOG_TRACE (OOM at WARN). Persistent consumers (UHCI frame list, interrupt pipes, sky2 rings, AHCI command tables, usbhid buffers) allocate once; transient consumers (`uhci_control` setup+QH+TDs, `enumerate_port` descriptor buffers, bcm5974 wellspring buffers) free on every path.

## Filesystem Check & Repair (`fsck` / `C:fsck`)

`kernel/dos/fsck.{c,h}` is a pluggable disk/filesystem interrogation and repair framework used by the `fsck` native command (`kernel/shell/cmd_fsck.c`). It operates directly on `BlockDev` sectors — deliberately independent of the mounted handlers' cached state — and is safe to run in read-only mode against mounted volumes (repair mode prints a mounted-volume warning first).

### Framework

- **`FsckCtx`** carries the output callback, check mode (`CHECK`/`REPAIR`/`INTERACTIVE`), `verbose`, `confirm` (y/n prompt), `brk` (Ctrl-C poll), and `yield` callbacks plus running error/warning/fixed counters. Checkers report through `fsck_err`/`fsck_warn`/`fsck_note`/`fsck_out`; every repair decision goes through `fsck_should_fix()` so INTERACTIVE mode prompts per fix.
- **`FsckFS` vtable** (`probe`/`info`/`check`) plus a dispatch table (`k_fsck_fs[]`) makes new filesystem checkers drop-in: implement `kernel/dos/fsck_<fs>.c`, add one table row. Registered probes: FAT32 (full checker in `fsck_fat32.c`), FAT12/16 (info + BPB sanity), ext2/3/4 (superblock info + dirty/error flags), Amiga FFS/OFS (full structural checker — see below), PFS/SFS, exFAT, NTFS, ISO9660 (info/interrogation only until their checkers land).
- **Whole-disk analysis** (`FSCK_DiskCheck`, used when `BlockDev.part_offset == 0`): MBR entry bounds/boot-flag/overlap checks with MBR rewrite repair, GPT header CRC/size/usable-range checks with CRC repair, and Amiga RDB `RDSK`/`PART`/`FSHD` chain walks with checksum repair. Disks with no recognised table fall through to the filesystem probe loop so bare OFS/FFS (and FAT32) media are checked too.

### Amiga OFS/FFS checker (in `fsck.c`)

`check_ffs` mounts the volume through the `ffs.c` driver (read/write since UAOS-89) and reports: dostype variant, bootblock checksum state (`valid` / `INVALID` / `not present (hardfile)` — the WinUAE convention of a zero checksum field is not an error; a stored-but-wrong checksum is repairable over blocks 0-1), root block number and volume name, bitmap validity and used-block count (advisory), then walks the whole directory tree via `FFS_DirIterNext` — checking block uniqueness through a visited bitmap, parent-key back-references, per-file data-pointer ranges and `T.LIST` extension chains, and reconciling declared byte size against the pointer count. Cycles and nesting are guarded (depth ≤ 32, bounded chains).
- **Media tools**: `FSCK_SurfaceScan` (batched read test reporting unreadable ranges, break/yield polled) and `FSCK_DumpSector` (hex+ASCII dump).
- All FAT scans and the cluster-reference bitmaps run against static aligned buffers (`g_scan` 64 KiB batches, two 512 KiB cluster bitmaps covering 4 Mi clusters); long loops yield and poll the break callback.

### FAT32 checker (`fsck_fat32.c`)

Seven phases: BPB validation (with sector-6 backup restore), FSINFO signature/counters, FAT1↔FAT2 compare (batched, retried reads — VirtIO can transiently fail under I/O bursts) with FAT2 resync repair, FAT entry scan (free/used/bad/out-of-range link truncation), iterative directory-tree walk (stack-based DFS, 8.3/LFN/attr/dot-entry sanity, per-entry cluster-chain marking for cross-link and loop detection, file size vs. chain-length reconciliation, clusterless-directory allocation + `.`/`..` rebuild), lost-cluster detection with optional reclaim, and free-space/FSINFO reconciliation. Repairs write every FAT copy and only count as `fixed` when the write succeeds.

### CLI

```
fsck DEVICE [CHECK|REPAIR|INTERACTIVE] [VERBOSE] [SURFACE] [DUMP=n] [INFO]
fsck LIST | fsck ALL
```

Partition BlockDevs run the FS checker; whole-disk BlockDevs run partition-table analysis; `ALL` iterates partition devices. Return codes via `failat`: 0 clean, 5 warnings, 10 errors found, 20 fatal/abort.

## Dynamic Handler Loading

Following the Amiga model, UAOS supports loading handlers from the `L:` directory. Built-in handlers include `ram_handler.c`, `fat_handler.c`, `ffs_handler.c`, `device_handler.c`, `aux_handler.c`, `port_handler.c`, `print_handler.c`, and `crossdos_handler.c`. Loadable handlers can be native or emulated M68k processes that handle specific device or filesystem logic.

### Print Handler (`print_handler.c`)

The PRT: print handler extends the generic `DeviceHandler` with LPT1 hardware output. When data is written to PRT:, bytes are strobed out to the host parallel port at I/O address 0x378. If no LPT1 is detected (e.g., in QEMU), data is buffered in the device ring buffer. The handler is registered as `print-handler` in the handler loader and can be accessed via the `print` shell command.

### CrossDOS Handler (`crossdos_handler.c`)

The CrossDOS handler provides read-only access to PC-format (MS-DOS FAT12/FAT16) floppy disks and small partitions. It implements:
- Boot sector probing and FAT type detection (FAT12 < 4085 clusters, FAT16 < 65525 clusters)
- FAT12/FAT16 cluster chain following with proper entry decoding (12-bit packed / 16-bit entries)
- Root directory scanning with 8.3 filename matching
- File read via cluster chain traversal
- AmigaDOS packet interface (ACTION_FINDINPUT, ACTION_READ, ACTION_END, ACTION_DISK_INFO)

Mounted via the `crossdos` shell command, which probes a block device and registers the volume in the DosList.

For more details on the packet handler design, see [Handler System](handler_system.md).

## Icon Engine (`icon_loader.c`)

The icon engine handles Amiga `.info` file reading, writing, and default icon generation.

### Icon Reader (`Icon_Load`)
Parses classic planar `.info` files from VFS into `ParsedIcon` structures. Converts interleaved bitplane data to ARGB for the linear framebuffer. Supports normal and selected image states, tool types, and default tool strings.

### Icon Writer (`Icon_Save`)
Serializes a `ParsedIcon` back to the Amiga `.info` binary format. Converts ARGB pixels back to planar bitplanes using the 4-color Workbench palette (transparent/white/black/grey). Writes the full DiskObject header, Gadget, Image structs, planar data, default tool string, and tool type BPTR array.

### Snapshot Write-back (`Icon_SavePosition`)
Updates `do_CurrentX`/`do_CurrentY` in an existing `.info` file without rewriting the entire file. If the `.info` doesn't exist, creates a minimal one with just the position. Used by the Workbench Snapshot menu action.

### Tool Type API
- `Icon_ToolTypeGet(icon, key)` — finds a tool type by key prefix (e.g. `"STARTPRI"` matches `"STARTPRI=5"`)
- `Icon_ToolTypeSet(icon, key, value)` — sets or replaces a tool type entry
- `Icon_ToolTypeDelete(icon, key)` — removes a tool type entry

### Default Icons / Pseudo-Icons (`Icon_MakeDefault`)
Generates procedural 4-color (depth=2) 32x32 pixel icons for each Workbench type:
- `WB_DISK` — floppy disk shape with label area
- `WB_DRAWER` — folder with tab
- `WB_TOOL` — page with folded corner and gear
- `WB_PROJECT` — page with text lines
- `WB_GARBAGE` — trashcan with lid
- `WB_DEVICE` — device box with LED
- `WB_KICK` — chip with pins

Selected state is auto-generated by inverting white↔black. Used for files without a `.info` file (pseudo-icons).
