---
type: Hardware Platform
title: MacBookPro4,1 bare-metal bring-up
description: Verified hardware inventory of the MacBookPro4,1 (Early 2008) test machine and the UAOS support needed to boot on it. Tracked in Plane under UAOS-131.
tags: [hardware, macbook, bare-metal, drivers]
timestamp: 2026-09-29T00:00:00Z
---

# MacBookPro4,1 as a UAOS target

Machine: `reaver@192.168.10.149` (SSH, passwordless sudo), currently Debian 13 /
kernel 7.1.8. DMI says **MacBookPro4,1** (not MacBook4,1), board Mac-F42C89C8.

## Verified inventory (lspci/lsusb/dmesg, 2026-09-29)

| Subsystem | Hardware | IDs / BARs | UAOS status |
|---|---|---|---|
| CPU | Core 2 Duo T9300, 2 cores, x86_64 | — | OK (BSP only) |
| Chipset | Intel PM965 + ICH8M | `8086:2a00` | CF8 PCI config works |
| Firmware | Apple EFI v1.1, **64-bit** | `fw_platform_size=64` | `build/bootx64.efi` usable; no BIOS unless CSM set up |
| Video | NVIDIA G84M GeForce 8600M GT, LVDS 1440x900 | `10de:0407` @01:00.0; FB @0xc0060000 | GOP → multiboot2 FB tag should work day-1; VBIOS at `~/workspaces/macbook41/mbp41-8600mgt-vbios.rom` (v60.84.49.03.00) |
| SSD | ICH8M SATA **AHCI** | `8086:2829` @00:1f.2, ABAR 0xdb504000 | `kernel/drivers/ahci.c` — **QEMU-verified** (detect+IDENTIFY+READ/WRITE DMA EXT, blockdev + partitions, MSI); real SSD pending |
| Optical | ICH8M PATA (PCI-native) | `8086:2850` @00:1f.1 | ide.c drives it (CD boot path verified in QEMU) |
| USB | 5× UHCI + 2× EHCI | `8086:2830–2835`, `2836`, `283a` | `kernel/drivers/uhci.c` + `usb.c` — **QEMU-verified** (enum, control, interrupt-IN); EHCI + hub support not yet implemented |
| Input | Internal kbd/trackpad = USB HID `05ac:0230`; trackpad needs bcm5974 init. **No PS/2** | UHCI bus, fs/ls | `kernel/drivers/usbhid.c` — **QEMU-verified** (kbd/mouse reports → shared PS/2-compat ring + `g_mouse`); bcm5974 mode-switch (UAOS-135) pending |
| Ethernet | Marvell Yukon-2 88E8058 PCIe GbE | `11ab:436a` @0c:00.0, BAR0 0xd7200000 | `kernel/drivers/sky2.c` written (Linux sky2 port, MSI-first/INTx); QEMU cannot emulate — real HW validation pending |
| Wi-Fi | Broadcom BCM4321 | `14e4:4328` | Deferred (firmware + 802.11) |
| Audio | ICH8 HD Audio (ALC885-class codec) | `8086:284b` @00:1b.0 | UAOS has AC97 only |
| Interrupts | IO-APIC GSI 16–21 INTx; MSI for sky2/AHCI/GPU; legacy PIC present but PIRQ routing unverified | MADT/MCFG/HPET in ACPI | `kernel/irq/{acpi,ioapic,irq}.c` — **QEMU-verified** (MADT parse, ISA overrides, PIT/RTC/PS2 via IO-APIC, AHCI MSI); PIC fallback retained |
| Debug | **No serial port** | — | telnetd (post-sky2) + on-screen klog |

## Work items (Plane: UAOS-131 umbrella)

- UAOS-132 Boot: EFI64 path + USB/CD media
- UAOS-133 Interrupt routing: IO-APIC + ACPI MADT/_PRT (blocks all INTx devices)
- UAOS-134 UHCI + EHCI host controller drivers
- UAOS-135 Apple bcm5974 multitouch trackpad init
- UAOS-136 AHCI SATA driver (SSD)
- UAOS-137 Marvell sky2 Ethernet driver (unlocks telnet debug)
- UAOS-138 USB HID class driver (keyboard/mouse input)
- UAOS-139 Video: GOP verify, BB_MAX for 1440x900, backlight PWM (BAR0+0x61c084), VBIOS/native modeset stretch
- UAOS-140 Intel HDA audio driver

## Ordering notes

1. Boot media + IO-APIC are the day-1 blockers; GOP video and the multiboot2
   sysroot image mean a kernel can reach the desktop before any disk driver
   exists.
2. UHCI+HID is required for *any* local input — no PS/2 fallback exists.
3. sky2 unlocks remote debugging (no UART), so it's high leverage early.
4. AHCI can be deferred: CD boot via existing ide.c + ISO9660 already provides
   a storage path; an ICH8M MAP-register IDE-compat escape hatch is unverified.
5. bcm5974 trackpad needs a mode-switch control transfer (Linux
   `drivers/input/mouse/bcm5974.c` is the reference).

## UHCI driver notes (lessons from QEMU bring-up, 2026-09-30)

- QEMU does not set PORTSC.PE after a port reset — the guest must write
  `PSC_PE | PSC_W1C` explicitly. Real HW is unaffected.
- The HC writes the completed TD's link pointer back into `qh->element`
  (observed in QEMU `uhci_process_frame`; UHCI spec behaviour). Persistent
  interrupt pipes must repoint `qh->element` at the TD when re-arming or
  the pipe dies after the first report.
- Re-arm must write token (toggle) *before* status (ACTIVE), reset CERR=3,
  and only flip the DATA toggle on successful completions.
- QEMU auto-inserts a `usb-hub` (0409:55aa, class 09) if devices aren't
  pinned to explicit root ports — use `port=1`/`port=2` in -device flags.
- QH/TD `status` fields must be `volatile` — HC writes them back
  asynchronously.
- **Do not enable USBINTR before the IRQ route exists.** QEMU's IO-APIC
  drops a level-triggered assert that arrives while the RTE is still
  masked-edge and never re-evaluates a pin left high — so interrupt TD
  completions during early enumeration latched USBINT but vector 43 never
  dispatched. Fix: `UHCI_SetupIRQs` writes USBINTR only after
  `IRQ_AttachPCI` returns. Real HW tolerates the early assert, but the
  deferred-enable ordering is correct for both.
- Multi-TD interrupt chains (needed for bcm5974's ~474-byte reports):
  intra-chain links need TD_LINK_VF (depth-first) or the HC executes only
  one TD per QH visit; IOC goes on the last TD only; a chain is done when
  the last TD clears ACTIVE, an earlier TD completes short (<mps), or a
  NAK/error bit sets.

## Boot-media / debug-console notes (2026-09-30)

- **Sysroot is a multiboot module, not the CD.** `scripts/build_iso.sh`
  packs `SYS_ROOT/` into `boot/uaos-sysroot.img` (ISO 9660, Rock Ridge)
  and GRUB loads it via `module2 ... uaos-sysroot`.
  `kernel/boot/mb2mod.c` finds the tag-3 module, registers a read-only
  RAM blockdev (`sysroot0`, 2048-byte sectors) over the GRUB-loaded
  memory, and `uaos_kernel_main.c` mounts `Workbench:` from it *before*
  the ATAPI/virtio-scsi scan.  This is what makes the USB-stick boot
  viable: the stick itself stays unreadable (no EHCI/MSC) but the root
  filesystem is already in RAM.  Module lands just above the kernel
  image (~0x90D0000 in QEMU); reads are lazy through the blockdev so the
  memory must not be reused.
- **`fbcon` cmdline flag = on-screen klog.** `kernel/klog/dbgcon.c`
  renders committed klog lines into the framebuffer — the only debug
  output on hardware with no serial port.  Enabled on the default GRUB
  entry (also the "FB Debug Console" menu entry).
- **fbcon must not touch VRAM before the MMU sandbox.** Bootstrap page
  tables only identity-map the first 1 GB and the framebuffer BAR sits
  above that on real GPUs.  `Dbgcon_Write` early-returns until
  `Dbgcon_VramReady()` runs right after `UAOS_MMU_Init()`; buffered
  klog lines are then replayed.
