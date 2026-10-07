---
type: Hardware Platform
title: MacBookPro4,1 bare-metal bring-up
description: Verified hardware inventory of the MacBookPro4,1 (Early 2008) test machine and the UAOS support needed to boot on it. Tracked in Plane under UAOS-131.
tags: [hardware, macbook, bare-metal, drivers]
timestamp: 2026-10-07T00:00:00Z
---

# MacBookPro4,1 as a UAOS target

Machine: `reaver@192.168.10.149` (SSH, passwordless sudo), currently Debian 13 /
kernel 7.1.8. DMI says **MacBookPro4,1** (not MacBook4,1), board Mac-F42C89C8.

## Verified inventory (lspci/lsusb/dmesg, 2026-09-29)

| Subsystem | Hardware | IDs / BARs | UAOS status |
|---|---|---|---|
| CPU | Core 2 Duo T9300, 2 cores, x86_64 | — | OK (BSP only). **UAOS-272:** idle heat came from the CPU parked at max FID/VID — the `Idle` task already `hlt`s (~100 % idle share via `taskstat`). `kernel/drivers/cpufreq.c` now drives EIST: `_PSS` from the DSDT (new `ACPI_Dsdt()` + minimal AML constant-package scan) else a synthesised same-VID high/low table, ondemand governor ticked at 100 ms from the PIT ISR, TM1 duty fallback when EIST is off/locked, `nocpufreq` boot flag to disable. **Gotcha:** `MSR_POWER_CTL` (0x1FC, C1E) does not exist on Core 2 — rdmsr #GP-panics at boot; CPUID.ECX.EST reads 0 under this firmware yet `MISC_ENABLE.16` is still writeable, so the driver trusts the MSR readback |
| Chipset | Intel PM965 + ICH8M | `8086:2a00` | CF8 PCI config works |
| Firmware | Apple EFI v1.1, **64-bit** | `fw_platform_size=64` | `build/bootx64.efi` usable; no BIOS unless CSM set up |
| Video | NVIDIA G84M GeForce 8600M GT, LVDS 1440x900 | `10de:0407` @01:00.0; FB @0xc0060000 | GOP → multiboot2 FB tag — metal-verified reaching the desktop at 1024x768; `gfxmode` now prefers `1440x900x32` (native panel mode) and `FB_Init` logs `[disp] fb: WxH@bpp … -> visible WxH` for mode recording. Backlight: `kernel/drivers/nv50bl.c` drives the SOR PWM (BAR0+0x61c084, duty 0–1025, NEW-commit — nouveau-verified contract) behind the `backlight [0-100]` shell command. VBIOS at `~/workspaces/macbook41/mbp41-8600mgt-vbios.rom` (v60.84.49.03.00) |
| SSD | ICH8M SATA **AHCI** | `8086:2829` @00:1f.2, ABAR 0xdb504000 | `kernel/drivers/ahci.c` — **metal-verified**: Hitachi HTS542525K9SA00 detected, RDB partitions `ahci01`/`ahci02` (DH0:/DH1:) registered, MSI vec 96 delivering, `ms_spin` TSC-calibrated (PIT ch2 gate) + IRQ handler acks PxIS/HBA IS (UAOS-224) |
| Optical | ICH8M PATA (PCI-native) | `8086:2850` @00:1f.1, I/O BARs 0x8108/0x811c/0x8100/0x8118/0x80e0 | `kernel/drivers/ide.c` — **metal-verified**: HL-DT-ST DVDRW GSA-S10N detected on native ch0, `atapi0`/`CD0:` blockdev (UAOS-275). Three metal-only layers fixed: PCI scan was fn-0/buses-0-3-only (this ctl is fn 1); native prog-if ignored BARs (ctl never decoded 0x1F0); and `ide_identify_device` treated `status==0` as absent — the SuperDrive genuinely idles at stat=0x00, so probe now classifies by post-reset signature regs (sc01/lba01/14/eb) before touching the command block. Waits are real-ms (port-0x80 spin) + PM cap forced to D0 |
| USB | 5× UHCI + 2× EHCI | `8086:2830–2835`, `2836`, `283a` | `kernel/drivers/uhci.c` + `usb.c` — **QEMU-verified** (enum, control, interrupt-IN); EHCI + hub support not yet implemented |
| Input | Internal kbd/trackpad = USB HID `05ac:021a` (**Geyser IV**, *not* bcm5974 `05ac:0230` as first assumed — live `UsbDev` readback on the box shows pid `0x021a`; Linux `appletouch` drives it) | UHCI bus, fs/ls | `kernel/drivers/{usbhid,appletouch,bcm5974}.c` — kbd types; the TP interface (proto2 mouse, ep1, mps 64) needs the **vendor-mode** switch (`atp_geyser_init`: read req1/write req9, wval `0x300`, `data[0]=0x04`) then streams 64-byte raw-sensor frames — Y sensors at bytes 1-15, X at 19-48 (triplets `-,v1,v2`), status byte 63 (bit0 button, bit2 baseline refresh). Deltas vs baseline → hump-count fingers + smoothed centroid; 1-finger click = left, 2 = right, 3+ = middle (latched till release). bcm5974 driver retained for `05ac:0230`-class Wellspring pads. **Metal-verified appletouch pipeline** (UAOS-135, after a long wobble hunt): line-faithful port of the Linux `atp_complete_geyser_3_4` path — single read+write mode switch, whole-pad smoothed centroid, 7/8 EMA on absolute position emitted only while census is unchanged, idle→reinit at 10 empty frames, baseline from BASE_UPDATE (they DO arrive — don't re-seed it yourself, that was the wobble root cause: re-seeding every ~80 ms baked a resting finger into the baseline) — plus an accumulating deadband of 2 pad units on the filtered delta for light-touch tremor. UHCI intr-IN chains resume on mid-chain NAK |
| Ethernet | Marvell Yukon-2 88E8058 PCIe GbE | `11ab:436a` @0c:00.0, BAR0 0xd7200000 | `kernel/drivers/sky2.c` (Linux sky2 port, MSI-first/INTx) — **metal-verified**: MSI vec 97, 1000baseT FD, DHCP/DNS/NTP/ping + telnetd live, SIGF_NET wake path confirmed (UAOS-137). QEMU cannot emulate this NIC |
| Wi-Fi | Broadcom BCM4321 | `14e4:4328` | Deferred (firmware + 802.11) |
| Audio | ICH8 HD Audio (ALC885-class codec) | `8086:284b` @00:1b.0 | UAOS has AC97 only |
| Interrupts | IO-APIC GSI 16–21 INTx; MSI for sky2/AHCI/GPU; legacy PIC present but PIRQ routing unverified | MADT/MCFG/HPET in ACPI | `kernel/irq/{acpi,ioapic,irq}.c` — **QEMU-verified** (MADT parse, ISA overrides, PIT/RTC/PS2 via IO-APIC, AHCI MSI); PIC fallback retained. **Fix:** DxxIP/DxxIR live in **RCBA MMIO** (LPC 00:1f.0 cfg 0xF0), *not* config space — earlier ECAM reads at 0x31xx were out of the 4 KB window and returned all-0xFF, misdiagnosed as "unrouted". `ich_route_gsi` now reads RCBA, programs unrouted DIR nibbles (INTA→PIRQA…), and programs unrouted PIRQA–H (cfg 0x60–0x67) → canonical GSI 16–23. **UAOS-174 fix:** DxxIR offsets are *not* uniform — D26IR is RCBA+0x**314C** (0x314A is a hole), D25IR=0x3150; the old `2*(31-dev)` formula misread/miswrote dev 26, silently unrouting both 00:1A.x UHCIs. `IRQ_Init` also sets OIC (RCBA+0x31FF) bit0 AEN — enables the internal IOxAPIC address decode (ICH8 datasheet 7.1.66, default 0) before `IOAPIC_Init` |
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
- **A NAKed TD keeps ACTIVE set (UAOS-226).** Real silicon (and QEMU's
  `TD_RESULT_NEXT_QH` path) writes back `status |= NAK` without clearing
  ACTIVE or advancing `qh->element`, and fires no IOC — the HC retries
  the TD every frame until a success clears `ACTIVE|NAK` and writes
  `actlen` (a 0-byte NAK reads back actlen=0x7FF on ICH8). So the scan
  must test NAK/error evidence *before* ACTIVE, a NAKed pipe needs no
  re-arm while `qh->element` still points at the resume TD, and NAKs do
  not cause interrupts. EHCI companions sharing a UHCI INTx with no
  driver are silenced via PCI command INTxDIS in `UHCI_SetupIRQs`.
- **The ~250 µs UHCI dispatch worst-case was the log line itself
  (UAOS-294).** `tickcheck` showed v48/v53 (the two UHCI vectors, one
  PIRQ-shared HC pair each) at ~210/251 µs max — exactly the two
  vectors that had logged `uhci: spurious irq count=1`. The handler
  emitted that WARN inside the dispatch; a klog line costs one polled
  UART write (~1-4 µs on real LPC port-IO) plus an fbcon glyph paint
  per character, so ~60 chars ≈ 200-300 µs. IRQ context now only
  records into `g_spur_pend`/`g_late_pend`/`g_mask_pend` per-GSI slots
  and `UHCI_DiagFlush()` formats the lines from the `usb-enum` task's
  100 ms round. While in there: a zero-status dispatch re-reads
  `USBSTS` once before declaring the assert foreign — a completion
  latched mid-dispatch is serviced immediately instead of waiting for
  the level line to re-fire (counted per-HC in `irq_late`, surfaced as
  `late=` in `usbdiag` and a deferred `latched mid-dispatch` line);
  zero-status on a vector shared with *foreign* handlers
  (`IRQ_VecShared(vec) > 1`) is the neighbour's IRQ and no longer
  counts as uhci-spurious at all (the 20k/s storm mask stays
  unconditional); and the old handler's unconditional `USBSTS` W1C
  write (an I/O-port write even on `st == 0`) is skipped now. The
  remaining spurious `count=1` per line is a genuinely foreign assert
  — typically a leftover device assert delivered the moment the GSI
  unmasks — benign. Metal-verified on the MBP4,1: gsi16 still logged
  its one `count=1`, but v48's worst dispatch fell 525k→21k cycles
  (~210→8.4 µs) and v53's 627k→11k (~251→4.4 µs); `late=0` on all 5
  HCs, no masks.
- **EHCI `CONFIGFLAG` must be cleared or ghost ports appear on UHCI
  (UAOS-225).** Firmware that ran its USB2 stack leaves EHCI `CF=1`,
  keeping every root port muxed to the (driverless) EHCI — the
  companion UHCI still reports `PORTSC.CCS`, so enumeration resets and
  polls a port whose data lines go elsewhere: control transfers retire
  with STALL/CRC/timeout (`td0=0x01450007` — SETUP went out, device
  never answered). `UHCI_Init` calls `ehci_release_ports()` first:
  scans PCI for EHCI (class `0x0C0320`), stops any live schedule
  (RS→0, waits `HCHalted`), clears `USBINTR`, writes `CF=0`, and
  settles 50 ms — every port then routes to its companion, HS devices
  falling back to full-speed. With no EHCI driver this is the
  documented USB-1.1-only hand-off; a real EHCI driver (UAOS-134)
  would replace it with proper `CF`+`PortOwner` handling.
- **Post-boot re-enumeration exists (UAOS-258/293).** A `usb-enum` task
  (`USB_StartEnumTask`, spawned after `TaskScheduler_Init`) re-probes
  connected-but-unenumerated ports at 100 ms cadence with bounded
  backoff (5 tries, 1→16 s), then parks. Parking is *not* terminal
  (UAOS-293): a connect-status edge (CCS flip or latched `PORTSC.CSC`
  via `uhci_port_csc`) revives the port instantly, and failing that a
  quiet heartbeat re-probe — a fresh port re-reset — runs every 60 s
  forever, since an SMC-gated device that powers up late raises no
  edge (CCS was already latched). The ERR park verdict and the DEBUG
  heartbeat line both print raw `PORTSC` (`psc=`) via the new optional
  `port_status` vtable op, so "connected+enabled but silent" vs
  "never finished enable" is visible in klog without `usbdiag`.
  `usb: late enum vid= pid=` still names whatever answers (BT HCI vs
  Apple IR `05ac:824x`). If it never wakes, the remaining fix is
  SMC/ACPI power control — not USB.
- **Deaf ports are quiet + cheap now (UAOS-262).** The retry loop used
  to emit ~50 verbose `ctrl fail` dumps and burn ~20 s per deaf port
  before parking. `uhci_control` early-outs after ~40 ms of zero TD
  progress, returns -2 for no-answer failures (timeout/CRC, NAK
  exhaustion, never-ran), and the dump is throttled to 2 verbose +
  power-of-two `xN` markers per HC. `enumerate_port` stops a round
  after 2 consecutive -2s; the "port deaf" park line is the single
  ERR-level verdict.

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

## Real-hardware bring-up findings (2026-09-30, first USB boot)

- **No i8042 — unbounded port loops wedge boot.** `PS2Kbd_Init` flushed
  OBF with `while (inb(0x64) & 1)` — port 0x64 floats at 0xFF with no
  PS/2 controller → infinite loop, boot stopped at "Initialising PS/2
  keyboard".  Same latent pattern in `uart_putchar`, ps2mouse's
  `_ser_putc`, and both RTC UIP waits — all now bounded spins.
  Since UAOS-278 the PS/2 init calls aren't even reached here: boot runs
  `PS2Ctl_Detect()` (i8042 self-test 0xAA→0x55; a floating status port
  reads 0xFF) and skips IRQ1/IRQ12 attach + port init when the
  controller is absent, logging "No i8042 controller" instead of the old
  phantom "PS/2 mouse active."
- **GPU framebuffer needs an MTRR WC entry or it is unusably slow.**
  Apple EFI leaves the 8600M GT aperture uncached; per-pixel writes were
  one bus transaction each → visible scanline-by-scanline drawing, and
  the old fbcon scroll did a byte-wise 5 MB read+copy per line.
  `kernel/exec/mtrr.c::MTRR_MarkWC` grabs a free variable-range MTRR and
  covers the fb range with a power-of-two WC region (safe: everything in
  the covered padding is still inside the VRAM aperture).  dbgcon now
  scrolls from a text ring and repaints — no VRAM reads.
- Input note: USB enumeration runs before the PS/2 stall point, and
  `USB_Poll()` ticks from the PIT — internal kbd/trackpad reports flow
  as soon as the scheduler starts; no PS/2 hardware is needed.

## MTRR ordering (2026-09-30, second USB boot)

- Boot with MTRR WC enabled garbled/froze the display right after GRUB
  finished loading the sysroot module — i.e. during the earliest kernel
  code.  `MTRR_MarkWC` originally ran right after `UAOS_MMU_Init` but
  *before* `IDT_Init`, so a `#GP`/`#UD` on any of the MSR accesses
  (`rdmsr`/`wrmsr`/`wbinvd`, or a firmware SMI interaction) triple-faults
  silently — no exception handler, no fbcon, frozen/garbled screen.
- Fix: `MTRR_MarkWC` now runs immediately after `IDT_Init()` in
  `uaos_kernel_main.c`, so a fault produces an on-screen exception dump
  (fbcon is live by then — VRAM-ready happens at MMU init).
- `nomtrr` cmdline token (new `Mb2_CmdlineHas` in mb2mod.c) skips MTRR
  setup entirely; GRUB entry "No MTRR (bisect)" uses it.

## Second USB boot (2026-09-30) — desktop reached

- MTRR relocation moved the garble later: normal boot now reaches the
  desktop on some runs, but **still intermittently garbles/freezes at
  the `MTRR_MarkWC` line** even with the full SDM sequence (MTRRs
  disabled via DEFTYPE.E=0 during the update).  `nomtrr` boots
  reliably — WC on this G84 framebuffer remains suspect; treat nomtrr
  as the bring-up default until proven otherwise.
- sky2 probes OK (bar0=0xD7200000, chip 0xB4 EC_U rev 3 = B0, real MAC,
  `phy_stat=0xAF80` = 1000 Mb/s full-duplex link up); AHCI MSI attaches
  (vector 96).  UHCI INTx unresolved — `IRQ_AttachPCI` can't map
  00:1a.x/00:1d.x via ACPI _PRT — falls back to PIT polling (functional).
- **sky2 TX stall #1 — missing EOP:** every `sky2_send` timed out with
  `done=0`.  The last LE of a packet must carry `EOP` (ctrl bit 7) or
  the TX engine never sees packet end.  Fixed: `le->ctrl = TX_LE_EOP`.
- **sky2 TX stall #2 — no RAM buffer:** diagnostic readback showed
  prefetch GET advancing (LEs fetched) but `STAT_PUT_IDX` stuck at 0 and
  `B2_E_0 = 0` (no internal RAM buffer).  On buffer-less chips Linux
  `sky2_mac_init` programs RX pause thresholds and sets
  `TX_STFW_ENA` (bit 30, TX_GMF_CTRL_T) — store-and-forward — which we
  skipped → frames fetched but never pushed to the wire.  Fixed.
- USB enumeration on metal: **4 devices / 7 ifs** — internal Apple
  `05ac:02xx` bound (`hid: kbd armed`, `bcm5974: trackpad + button
  armed`); one port fails GET_DESCRIPTOR(8) — later found to be
  EHCI-owned ghost attach (UAOS-225, fixed via `CF=0` release —
  see UHCI driver notes).  Input at the desktop still unverified.
- TX-timeout diagnostic dump retained (get/stput/qcsr/hwe/pfctl/stctl/
  st0/ram) — bisects fetch vs status vs error stages.

## sky2 vs working Linux register diff (2026-09-30)

A full live `ethtool -d` register dump + `lspci -xxx` config space was
captured from Debian on the *same* MacBook (eth0 up, 1Gbps FD, DHCP OK).
Golden references saved to `~/workspaces/macbook41/`:
`sky2-regdump-linux-working.hexdump.txt`, `sky2-pci-config-linux.txt`.

- `B0_IMSK` is load-bearing on this silicon: with all device IRQs masked
  the status BMU produces *nothing*; unmasking `Y2_IS_BASE|Y2_IS_PORT_1`
  (`0xC000001D`, same as Linux) made `STAT_PUT_IDX` advance one entry
  per TX.  INTx needn't be routed — the mask itself gates the unit.
- `STAT_LIST_ADDR_LO` ignores writes for a window after
  `SC_STAT_RST_CLR` on this rev — first write reads back 0.  A
  write-verify poll until it sticks is required (added in
  `sky2_status_init`).  ASF ruled out (`B28_Y2_ASF_STAT_CMD` reads 0).
- Register-level parity is now essentially total: STAT_CTRL/ADDR/
  LAST_IDX, prefetch ctrls, BMU CSR (`0x11AA` = RST_CLR|OP_ON|FIFO_ENA|
  FIFO_OP_ON|START|DIS_RX_CHKSUM), GMAC ctrl `0x1800`/serial `0x221E`/
  TX param `0xD7C4` all match Linux live values.  The `0x2xxxx` deltas
  in prefetch/status ctrl readbacks are internal latch bits, not knobs.
- **Root cause candidate — PCIe DevCtl NoSnoop left at reset default:**
  our fixup only masked MRRS (`dc & ~0xF000`), leaving bit 11
  (Enable No Snoop) and bit 4 (Relaxed Ordering) at their PCIe-reset
  value of 1 — DevCtl ≈ `0x481F` vs Linux's `0x400F`.  With NoSnoop
  set, chip DMA reads *and* writes skip CPU-cache snooping: prefetched
  descriptors could come back as stale DRAM zeros → `CHK_TXA1`/`CHK_RX1`
  descriptor-check errors seen in `B0_Y2_SP_ISRC2=0x1D`; chip status
  writebacks land in DRAM but the CPU keeps reading cached zeroed
  ring lines → `STAT_PUT_IDX` advances, `sfl`/`aer`=0, ring stays zero;
  later dirty-line eviction overwrites the chip's entries → the
  0–384 MB memory scan found no leaked entries anywhere.  Fixed:
  DevCtl mask now clears `0xF810` (MRRS field + NoSnoop + RelOrd) with
  a write/readback log (`devctl=0x…->0x400f` expected).  Belt-and-
  braces: `wbinvd` in the TX-timeout dump before re-reading the status
  slot (`f00=` post-flush dword) — nonzero there with `s00=0` is the
  coherency smoking gun if NoSnoop somehow persists.
- CHK-clear probes added: `BMU_CLR_IRQ_CHK` written to both queues at
  end of init (`sp_init=` logged); timeout dump prints `sp=` so a
  *re-fired* CHK bit after the clear = active descriptor rejection.
- PCI config deltas not yet replicated: ASPM L0s/L1 (Linux enables
  both; we leave at firmware default).
- Under Linux the UHCIs route via IO-APIC GSIs 16/18/20/21 — the routes
  exist; our ACPI `_PRT` resolution misses them (PIT polling covers it
  for now).  sky2 = MSI vector, AHCI = MSI-30.
- Kernel identity-maps all 4 GB (2 MB pages); DMA buffers live below
  4 GB — VA==PA confirmed, no truncation (`bufhi=0`).

## sky2 ring alignment (2026-09-30, third USB boot)

- DevCtl NoSnoop/RlxdOrd cleared to match Linux `0x400F` and a
  `wbinvd` added before re-reading the status slot: still zero →
  cache coherency ruled out.
- The golden diff showed TX prefetch base `0x06D8` and status base
  `0x0E88` **both = 0x031CD000**, although the rings were allocated
  0x200 apart (64-byte aligned); RX base `0x0458` = 0x031CC000.  The
  base registers drop bits [11:0], so the chip fetched LEs from and
  wrote status to the wrong addresses (also explains `rxw=0`,
  `B0_ISRC=0x18` MAC RX-overrun pending, `STAT_TXA1_RIDX=3`).
- Fix: rings allocated with `SKY2_RING_ALIGN` = 32 KB (FreeBSD msk
  `MSK_RING_ALIGN`/`MSK_STAT_ALIGN`); `sky2_prefetch_init` warns on a
  base readback mismatch.  **Result: DHCP lease on metal
  (192.168.10.149).**

## sky2 TX wrap + re-entrancy (2026-09-30, fourth USB boot)

- TX died at the first ring wrap: `done=0x40 want=0x00`, prefetch
  GET=0x5C with a 64-LE ring — the chip ran past LAST_IDX 0x3F.  Linux
  uses 0x7F; TX ring is now 128 LEs and prefetch init warns if
  LAST_IDX doesn't read back.
- `sky2_send` drains the status ring while waiting → RX callback →
  ARP/ICMP/TCP reply → nested `sky2_send`.  Fixed: completion test is
  "reached or passed" (not equality); TX frames copied into per-LE
  driver buffers; RX frames copied to a bounce buffer and the RX
  buffer recycled *before* the callback, delivered only at drain depth
  1 (nested drains recycle without delivery, so buffer order can't
  permute — suspected source of `[net] rx: bad length`); IRQ/poll
  skip draining while a thread drain is active.
- Per-status-entry `st#` log moved to TRACE (it flooded fbcon).

## sky2 concurrency redesign (2026-09-30, fifth USB boot)

- DHCP, DNS (pool.ntp.org) and NTP all worked; telnetd accepted one
  connection, then silence, and no further connections.  The
  drain-depth gate above was the culprit: a task preempted/sleeping
  inside the RX callback left depth>0, so every poll returned early,
  the IRQ skipped draining, and nested drains dropped RX — permanent
  RX stall.
- Current model: `sky2_drain_status` (cli-protected; called by send,
  poll and the IRQ) only records completions — TX index + an in-order
  pending-RX counter with per-buffer lengths.  `sky2_rx_deliver` runs
  only from `sky2_poll` (task context): pop + copy to stack + resubmit
  under cli, then callback with IRQs on.  No global gate; TX LE claim
  + PUT write under cli.
- TX-timeout diag no longer reads `B0_Y2_SP_ISRC2` (reading it masks
  device IRQs until `LISR` is read) and no longer scans memory; golden
  diff prints once per boot.  `sky2_setup_irq` is idempotent (MSI was
  attached twice, vectors 97 and 98).

## Event-driven RX wake (2026-09-30, post-telnet)

With telnet live on 192.168.10.149, the remaining latency was the
100 Hz sleep-poll cadence: pump + listener each did
`net_stack_poll(); Task_SleepTicks(1)` → up to ~10 ms added per
keystroke/line, and `send_buf`/`remote_send_raw` busy-spun.

- `Signal()` is now IRQ-safe: pushfq/cli ... popfq instead of
  cli ... unconditional sti.  Callers already worked around the old
  semantics (shell_win.c's keyboard feed notes it), so restoring IF
  is strictly safer, and IRQ handlers can now signal tasks directly.
- New `SIGF_NET` (bit 7 was reserved, unused): `net_rx_notify_arm /
  disarm / kick` in stack.c; armed tasks get `Signal(SIGF_NET)` from
  the driver IRQ when RX work is recorded.
- `Task_WaitTicks(sigset, ticks)` — Wait for signal bits OR a tick
  bound, whichever comes first (Task_SleepTicks only waits on ticks;
  plain Wait would miss the watchdog cadence).
- sky2: `sky2_drain_status` returns whether it recorded RX; the IRQ
  handler calls `net_rx_kick()` + `Task_ScheduleFromIRQ()` so an
  armed net task preempts at IRQ-exit (isr_common honors
  Task_SwitchNext on any vector).  `sky2_set_notify` setter plumbed
  through `net_device.c`.
- Consumers rewired: telnetd listener + pump + `send_buf`, and
  `remote_send_raw` in shell_win.c — `poll; Task_WaitTicks(SIGF_NET,1)`
  keeps watchdogs bounded while dropping the RX→wake latency to ~0.
- `net_rx_notify_disarm(Task_Current())` in `Task_Exit` so a dead
  task can't be signalled later (same pattern as usock cleanup).
- e1000 + virtio_net IRQ handlers kick the same path.
- `tcp_listen` now refuses duplicate binds on the same port.
- QEMU-verified: virtio DHCP/DNS/NTP + repeated telnet connect /
  dmesg-flood / disconnect cycles clean.
- Still open: tcp_send's one-in-flight-segment cap limits throughput
  to ~1 MSS/RTT; USB-HID input is poll-driven (UHCI IRQs unresolved).

## MTRR / framebuffer WC (2026-09-30, nomtrr workaround still needed)

Two real bugs found in `MTRR_MarkWC` + one gap:

1. **`MTRR_TYPE_WC` was `0x06`** — that's Write-Back, not WC (UC=0,
   WC=1, WT=4, WP=5, WB=6).  The framebuffer was being marked WB.
   Fixed to `0x01`.
2. **Missing cli + CR4.PGE + TLB flush** — the SDM §11.11.8 sequence
   needs interrupts masked for the cache-disabled window (the IDT
   isn't up at the call site, so any fault = triple fault) and a
   PGE toggle + CR3 reload so stale global translations don't pin
   the old memtype.  Both added, with IF save/restore.
3. **Overlap check** — two VALID variable ranges overlapping with
   different types are *undefined* on Intel (hang).  Now dumps every
   occupied slot at boot and either no-ops if the region is already
   WC, or skips with a log line rather than corrupting ordering.

If the boot still hangs with MTRR enabled, the `[MTRR] base/slot/cr4`
and per-slot dump lines localise it.

## Display glitch follow-up (2026-10-02, UAOS-187..194)

The photographed artifacts (stray/duplicated desktop pixels tracking the
mouse) had software causes that are now fixed — the leading suspect was a
torn cursor commit (`cursor_commit_draw` read the IRQ-updated position
three separate times), plus IRQ-context framebuffer writes from the RTC
blanker path and unserialised `WM_Redraw` frames from preemptible tasks.
All render is now pump-serialised and cursor commits latch the position
atomically (UAOS-189..193). **Metal-verified same day (UAOS-194):**
booted the normal (MTRR WC) entry — desktop rendering is smooth with no
artifacts during mouse movement, so the photographed glitches were fully
explained by the software races, not WC ordering on the G84. WC is no
longer considered suspect for this symptom; `nomtrr` remains available as
a diagnostic escape hatch but is not needed. Bonus: once
`g_bb_coherent` is set the cursor path never reads VRAM, which also
helps any future `nomtrr` boot.
## Transient false-color corruption (2026-10-05, UAOS-265)

The audit's pink/magenta full-screen frames below a horizontal boundary
(faint ghosted window/text outlines, self-healing on repaint) traced to
stale Intuition screen/window ownership, not hardware: an M68k task's
guest `BitMap`/`ColorMap` lives in its private RAM window, and after the
task exited its screen slot stayed `active`/`is_front`, so the planar
emit decoded freed or foreign memory through ambient `g_ram` — the
nibble-quantized OCS palette in the corrupt frame was a foreign ColorMap.
`UAOS_Intuition_CleanupTask()` now retires the dying task's slots before
its window is released (`Task_Exit`/`stub_RemTask`), and every emit path
guards orphaned owners. QEMU-verified via `Demos:LeakTest` (opens a
screen+window, exits) across repeated cycles; metal re-verify pending.

## Video: native mode + backlight (2026-10-07, UAOS-139)

- GOP → multiboot2 → `FB_Init` was already proven on metal (desktop
  reached in prior USB boots) — GRUB was picking `1024x768x32`, the
  first entry in `gfxmode`.  `scripts/grub.cfg` now lists
  `1440x900x32` first in both EFI and legacy branches so the firmware
  selects the LVDS panel's native mode when GOP offers it; fallbacks
  unchanged.  `BB_MAX_W/H = 1440x1024` already covers 1440x900 — no
  back-buffer change needed (if a mode larger than 1440x1024 arrives,
  `FB_Init` still clamps and the desktop renders top-left).
- `FB_Init` now emits `[disp] fb: <tag W>x<tag H>@<bpp> pitch=… addr=…
  -> visible <W>x<H>` — the achieved-mode record for metal bring-up
  (`sysinfo`/`version` also report the visible geometry).
- New `kernel/drivers/nv50bl.c`: NVIDIA NV50+ backlight via the SOR
  PWM regs (`BAR0+0x61c084+i*0x800`, bit31 NEW commit, duty 0–1025).
  Probe scans PCI for `10de` + display class, takes BAR0 (<4G,
  identity-mapped), picks the first SOR with nonzero CTL, and never
  writes anything itself — only the `backlight` shell command does
  (`backlight` reports, `backlight 60` sets).  Contract identical to
  nouveau / mbp_nv50_bl (`~/workspaces/macbook41/mbp41-nv50-backlight/
  okf/register-interface.md`).  QEMU-verified no-op path
  ("no supported GPU PWM found"); smoke.sh 24/24.
- Metal verification pending: machine unreachable on
  192.168.10.149/.176 at change time.  On next USB boot check the
  `fb:` klog line for 1440x900 and try `backlight 50`.
- Deferred per card scope: VBIOS int10 (needs x86 emu or CSM boot) and
  native G84 modesetting — both major efforts, VBIOS reference at
  `~/workspaces/macbook41/mbp41-8600mgt-vbios.rom`.
