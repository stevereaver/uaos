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
| SSD | ICH8M SATA **AHCI** | `8086:2829` @00:1f.2, ABAR 0xdb504000 | No driver — needs AHCI |
| Optical | ICH8M PATA (PCI-native) | `8086:2850` @00:1f.1 | ide.c may already drive it (CD boot path) |
| USB | 5× UHCI + 2× EHCI | `8086:2830–2835`, `2836`, `283a` | No USB stack at all |
| Input | Internal kbd/trackpad = USB HID `05ac:0230`; trackpad needs bcm5974 init. **No PS/2** | UHCI bus, fs/ls | ps2kbd/ps2mouse dead here |
| Ethernet | Marvell Yukon-2 88E8058 PCIe GbE | `11ab:436a` @0c:00.0, BAR0 0xd7200000 | Needs sky2-class driver |
| Wi-Fi | Broadcom BCM4321 | `14e4:4328` | Deferred (firmware + 802.11) |
| Audio | ICH8 HD Audio (ALC885-class codec) | `8086:284b` @00:1b.0 | UAOS has AC97 only |
| Interrupts | IO-APIC GSI 16–21 INTx; MSI for sky2/AHCI/GPU; legacy PIC present but PIRQ routing unverified | MADT/MCFG/HPET in ACPI | Kernel is 8259 PIC + ExtINT only |
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
