# Running Classic Amiga Applications on UAOS

A practical bring-up playbook for launching 68k Amiga Hunk binaries under
UAOS's Musashi-based emulation, using **OctaMED Soundstudio V5** as the
worked flagship example.

---

## 1. What works today

OctaMED V5 launches, opens its custom 640×768 tracker screen, renders the
real UI (menu bar, button row, tracker grid, instrument list), processes
IDCMP input (mouse, menus, IntuiTicks), opens its `.med`/instrument files
through `dos.library`, and drives `audio.device`/`ciaa.resource`
interrupt setup — all on real AmigaOS semantics, not stubs.

Verified end-to-end in QEMU (see §5 for the test list).

## 2. Preparing the disk image

The test harness boots OctaMED from a second raw HDF (`build/octamed.img`)
alongside the system disk. To stage a real install:

```bash
# create a 64 MB image and format it FAT32
dd if=/dev/zero of=build/octamed.img bs=1M count=64
mkfs.vfat -F 32 -n OCTAMED build/octamed.img
mcopy -i build/octamed.img -s <octamed_dir>/Octamed ::Octamed
```

The image must expose `OCTAMED:OCTAMED/OCTAMED.V5` — the shell resolves the
`OCTAMED:` volume assign from the FAT volume label.

## 3. Booting and launching

```bash
bash scripts/build_iso.sh
bash scripts/run_with_disk.sh build/uaos_disk.qcow2 build/octamed.img
```

At the Workbench shell window (or over telnet, `localhost:2323`):

```text
run OCTAMED:OCTAMED/OCTAMED.V5
```

OctaMED shows a first-run `Request` requester (missing `S:` config files —
expected) — click **OK** — then opens its custom screen. The screen title
bar shows `CPU`/`Free`/clock readouts; the main window renders the tracker
layout.

### What to expect

- **640×768 custom screen** — OctaMED's native mode, not the Workbench.
- **Modal requesters** — missing config files produce "object not found"
  requesters; each is a real Intuition requester, dismissed with a click.
- **CPU meter reads high** — a vblank-driven tracker UI repaints
  continuously; the task *does* block between ticks (this is normal for
  the compositing model, not a stuck spin).

## 4. Debugging a bring-up failure

### Quick triage

| Symptom | First check |
|---|---|
| Task never appears | `[dos]` serial lines — did `hunk_load`/`run` resolve the path? |
| Window opens, no content | `[disp]` gadget dump — did `OpenScreen`/`OpenWindow` return real structs? |
| `WILD-PC pc=0` | `cb=0x…/0x… k=N` in the log names the callback entry that went wild |
| Task spins 100 % | `g_m68k_block_marks` via GDB — a frozen counter means a tight guest loop |
| BOOPSI object fails | `[cls]`/`find_public_class` — class ID resolving to 0 is a mirror problem |

### GDB hooks

Every QEMU test launches a GDB stub (`-gdb tcp::<port>`). The kernel ELF is
`build/uaos-kernel.elf`. Each m68k task owns a private `g_ram` window —
`task->m68k_ram` is the host pointer; a guest address `X` lives at
`m68k_ram + X`. **Never read guest memory from a different task's window.**

Useful symbols:

- `g_m68k_first_wild_pc` / `g_m68k_first_wild_prev` — first PC ≥ 0x10000
  (the decruncher entry for packed binaries — not itself a fault).
- `g_m68k_block_marks` — increments on every blocking wait; a frozen value
  means the guest is spinning.
- `g_m68k_last_cb_{entry,data,kind}` — last host-dispatched guest callback
  (kind: 1=hook, 2=isr, 3=putch, 4=Supervisor).
- `g_m68k_pc_ring` — recent non-sequential PC edges; a tight loop fills it
  with the loop's own jump edges.

### Reading the serial log

- `[trace] #N lib=L fn=F` — every LVO call (first ~700, then every 10⁴th).
- `[lib] <name> lvo=-N` — a call to a library with no mapped function.
- `[icr]`/`[irq]` — interrupt-vector registration and delivery.
- `[watch]` — (when `g_watch_mem` is poked to 1) per-4KB-page divergence
  in the packed-hunk band, with the live PC — catches a host-side writer
  scribbling on the guest arena.

## 5. Regression suite

```bash
python3 tests/qemu_octamed_ui_test.py     # launch, requester, 640x768 shot
python3 tests/qemu_m68k_lifecycle_test.py # spawn/block/soak/teardown, 24 checks
python3 tests/qemu_octamed_iff_test.py    # iffparse + instrument load path
python3 tests/qemu_layout_test.py         # BOOPSI layout.gadget + input
python3 tests/qemu_idcmp_input_test.py    # IDCMP classes, IntuiTicks, menus
python3 tests/qemu_asl_test.py            # ASL file requesters
python3 tests/qemu_wblaunch_test.py       # Workbench icon-launch path
python3 tests/qemu_exec_coverage_test.py  # exec.library call surface
python3 tests/qemu_devio_test.py          # device I/O (timer etc.)
python3 tests/qemu_optlibs_test.py        # optional-library open surface
```

Stale QEMU processes hold the monitor sockets and GDB ports — `pkill -f
qemu-system` plus remove `/tmp/uaos_*_mon` before rerunning after a crash.

## 6. Known limitations (as of this writing)

- **`AddTask`** links a child task on `TaskWait` but does not schedule a
  second m68k context — single-context-per-window model.
- **OctaMED's `Supervisor()` hardware-introspection routine** reads
  `audio.device` internals; fields that are 0 in the generated device base
  produce absorbed wild-PCs (recovered, non-fatal).
- **OctaMED keyboard shortcuts** (e.g. Shift+I → instrument requester)
  use a lower-level input path than our IDCMP rawkey delivery — menu/click
  input works; shortcut keys are not yet wired.
- **`AddTask`/`SystemTagList` synchronous-only** — `SYS_Asynch` is parsed
  but children queue for sequential run, not true pre-emption.

---

*See also: `documentation/manual.md` (architecture),
`okf/emulation/index.md` (ABI correctness notes), `docs/` QEMU harness.*
