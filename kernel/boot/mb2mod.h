/*
 * mb2mod.h — Multiboot2 module helpers
 *
 * GRUB loads each `module2` line into RAM and records it in the
 * multiboot2 info structure as a type-3 tag (mod_start, mod_end,
 * NUL-terminated cmdline).  The "uaos-sysroot" module carries an
 * ISO 9660 image of SYS_ROOT built at ISO-build time, so Workbench:
 * can mount from RAM when no CD-ROM block device is present (e.g. a
 * USB-booted MacBookPro4,1 where the stick needs EHCI+mass-storage).
 */

#ifndef UAOS_MB2MOD_H
#define UAOS_MB2MOD_H

#include <stdint.h>
#include "dos/blockdev.h"

/* Find a multiboot2 module by its cmdline string (exact match).
 * Returns 1 and fills start/size (byte extent) on success. */
int Mb2_FindModule(uint32_t mb2_info_phys, const char *name,
                   uint64_t *start, uint64_t *size);

/* Returns 1 if the kernel command line (tag 1) contains the token. */
int Mb2_CmdlineHas(uint32_t mb2_info_phys, const char *tok);

/* Locate the "uaos-sysroot" module and register a read-only RAM block
 * device named "sysroot0" over it (2048-byte sectors, matching the ISO
 * image).  Returns the registered BlockDev, or NULL when the module is
 * absent.  The module memory must stay valid — it backs all later
 * reads of Workbench: proxy files. */
BlockDev *Mb2Mod_RegisterSysroot(uint32_t mb2_info_phys);

#endif /* UAOS_MB2MOD_H */
