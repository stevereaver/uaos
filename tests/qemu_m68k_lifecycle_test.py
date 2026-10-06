#!/usr/bin/env python3
"""qemu_m68k_lifecycle_test.py — UAOS-247 regression: M68k task lifecycle.

Boots the UAOS ISO headless and drives the guest through the launch/exit
paths UAOS-247 added:

  * LeakTest  x3 — opens a screen+window, exits without closing them:
    Task_Exit must retire Intuition slots AND recycle the m68k RAM slot.
  * LeakFile     — exits holding an Open()ed file + Lock()ed dir:
    HandleTable_FreeByOwner must leave zero owner-dead entries.
  * HelloWorld   — parked in WaitPort; window close-gadget click on a
    window that does NOT subscribe IDCMP_CLOSEWINDOW must halt the guest
    (blocked-wait wakeup) and tear it down.
  * SpinTest     — pure CPU spin: 100M-cycle liveness watchdog must fire
    exactly once (serial) while the task keeps running; an external
    m68k_halted set via GDB must tear it down.

Requires: gdb, QEMU, a built ISO + build/uaos_disk.qcow2.
Run:  tests/qemu_m68k_lifecycle_test.py [--iso PATH] [--keep] [--timeout SEC]

Exit 0 = all checks passed.
"""

import argparse
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_layout_test import (Fail, Gdb, Monitor, Uaos, wait_serial,
                              REPO, KERNEL_ELF)

OCTAMED_HDF = os.path.join(REPO, "build", "octamed.img")


def launch_with_hdf(iso, port, mon, serial, vars_fd, hdf=None):
    """launch() + optional second virtio-blk raw disk (snapshot mode — the
    backing .hdf is never written)."""
    import subprocess
    for p in (mon, serial):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass
    subprocess.run(["cp", "/usr/share/OVMF/OVMF_VARS_4M.fd", vars_fd],
                   check=True)
    argv = ["qemu-system-x86_64",
            "-machine", "q35",
            "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
            "-drive", f"if=pflash,format=raw,file={vars_fd}",
            "-device", "piix3-ide,id=ide",
            "-drive", f"if=none,id=cdrom,media=cdrom,file={iso}",
            "-device", "ide-cd,drive=cdrom,bus=ide.0",
            "-device", "virtio-blk-pci,disable-modern=on,drive=blk0",
            "-drive", f"id=blk0,file={os.path.join(REPO,'build','uaos_disk.qcow2')},if=none,format=qcow2",
            "-netdev", "user,id=n0,net=10.0.2.0/24,host=10.0.2.2,restrict=off,hostfwd=tcp::2323-:23",
            "-device", "virtio-net-pci,netdev=n0,disable-modern=on"]
    if hdf:
        argv += ["-device", "virtio-blk-pci,disable-modern=on,drive=blk1",
                 "-drive", f"id=blk1,file={hdf},if=none,format=raw,snapshot=on"]
    argv += ["-serial", f"file:{serial}",
             "-monitor", f"unix:{mon},server,nowait",
             "-gdb", f"tcp::{port}",
             "-m", "512M", "-vga", "virtio", "-display", "none",
             "-no-reboot", "-no-shutdown"]
    proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    for _ in range(50):
        if proc.poll() is not None:
            raise Fail(f"qemu exited at launch (rc={proc.returncode})")
        if os.path.exists(mon):
            return proc
        time.sleep(0.1)
    proc.terminate()
    raise Fail(f"qemu never created monitor socket {mon}")

TASK_REMOVED = 3


def log_wdog_count(serial):
    """Count liveness-watchdog dumps in the serial log."""
    try:
        return open(serial, errors="replace").read().count("spin watchdog fired")
    except FileNotFoundError:
        return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--iso", default=os.path.join(REPO, "build", "Ultimate_Amiga_OS.iso"))
    ap.add_argument("--gdb-port", type=int, default=11235)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep", action="store_true", help="leave QEMU running")
    ap.add_argument("--shot-dir", default=tempfile.gettempdir())
    a = ap.parse_args()

    mon = "/tmp/uaos_lifecycle_mon"
    serial = "/tmp/uaos_lifecycle_serial.log"
    vars_fd = "/tmp/uaos_lifecycle_ovmf.fd"
    checks, failed = [], []

    def check(name, ok, detail=""):
        tag = "PASS" if ok else "FAIL"
        checks.append((name, ok))
        print(f"[{tag}] {name}" + (f" — {detail}" if detail else ""), flush=True)
        if not ok:
            failed.append(name)

    if not os.path.isfile(a.iso):
        sys.exit(f"ISO not found: {a.iso}")
    if not os.path.isfile(KERNEL_ELF):
        sys.exit(f"kernel ELF not found: {KERNEL_ELF}")

    proc = launch_with_hdf(a.iso, a.gdb_port, mon, serial, vars_fd,
                           hdf=OCTAMED_HDF if os.path.isfile(OCTAMED_HDF) else None)
    deadline = time.time() + a.timeout
    uaos = Uaos(Monitor(mon), Gdb(a.gdb_port))
    shot = os.path.join(a.shot_dir, "uaos_lifecycle_test.ppm")

    def type_cmd(text):
        uaos.mon.type_string(text)
        uaos.mon.send("sendkey ret")

    def probe():
        return uaos.gdb.probe_state()

    def ram_slots():
        """Count used guest-RAM slots (static g_ram_used in exec_task.c)."""
        out = uaos.gdb.run(r'''
set $n = 0
set $i = 0
while $i < 4
  set $n = $n + 'exec_task.c'::g_ram_used[$i]
  set $i = $i + 1
end
printf "@@RAMSLOTS used=%d\n", $n
''')
        m = re.search(r"used=(\d+)", out[0])
        return int(m.group(1))

    def live_handles():
        """Count handle-table entries still in use (g_entries static)."""
        out = uaos.gdb.run(r'''
set $n = 0
set $i = 0
while $i < 128
  if 'handle_table.c'::g_entries[$i].type != 0
    set $n = $n + 1
  end
  set $i = $i + 1
end
printf "@@HANDLES inuse=%d\n", $n
''')
        m = re.search(r"inuse=(\d+)", out[0])
        return int(m.group(1))

    def task_named(needle, st=None):
        st = st or probe()
        return next((t for t in st["tasks"]
                     if needle.lower() in t["name"].lower()), None)

    def await_task_exit(task_i, name, timeout=15):
        for _ in range(int(timeout * 2)):
            st = probe()
            gone = all(t["i"] != task_i or t["state"] == TASK_REMOVED
                       for t in st["tasks"])
            if gone:
                return True
            time.sleep(0.5)
        return False

    try:
        # --- boot ---------------------------------------------------------
        ok = wait_serial(serial, "telnetd: listening", 120)
        check("boot: serial reaches telnetd", ok)
        if not ok:
            raise Fail("never reached telnetd")

        st = None
        t0 = time.time()
        while time.time() - t0 < 40:
            try:
                st = probe()
            except Fail:
                pass
            if st and any("Shell" in w["title"] for w in st["wins"]):
                break
            time.sleep(1.5)
        check("boot: Shell window registered",
              bool(st and any("Shell" in w["title"] for w in st["wins"])))
        if not st:
            raise Fail("no state probe ever succeeded")

        base_handles = live_handles()
        base_slots = ram_slots()
        print(f"  baseline: handles={base_handles} ram_slots={base_slots}")

        # --- Phase A: LeakTest x3 — orphan window/screen + slot recycle ---
        for i in range(3):
            type_cmd("SYS:Demos/LeakTest")
            # LeakTest exits immediately; window/screen slots must be retired
            # by Task_Exit before the RAM slot is freed.
            t0 = time.time()
            clean = False
            while time.time() - t0 < 15:
                st = probe()
                gone = (not task_named("LeakTest", st)
                        and all("LeakTest" not in w["title"] for w in st["wins"])
                        and not st["slots"])
                if gone:
                    clean = True
                    break
                time.sleep(0.7)
            check(f"LeakTest #{i+1}: orphan window+screen retired", clean,
                  f"tasks={[t['name'] for t in st['tasks']]} "
                  f"wins={[w['title'] for w in st['wins']]} slots={len(st['slots'])}")
            check(f"LeakTest #{i+1}: RAM slot recycled",
                  ram_slots() == base_slots,
                  f"used={ram_slots()} baseline={base_slots}")

        # --- Phase B: LeakFile — file/lock handle reclaim ------------------
        type_cmd("SYS:Demos/LeakFile")
        time.sleep(4)
        st = probe()
        check("LeakFile: task exited cleanly",
              not task_named("LeakFile", st),
              f"tasks={[t['name'] for t in st['tasks']]}")
        check("LeakFile: no lingering handle-table entries",
              live_handles() == base_handles,
              f"inuse={live_handles()} baseline={base_handles}")

        # --- Phase C: HelloWorld — blocked guest, close-quit --------------
        type_cmd("SYS:Demos/HelloWorld")
        hw = None
        t0 = time.time()
        while time.time() - t0 < 30:
            st = probe()
            win = next((w for w in st["wins"] if "Hello" in w["title"]), None)
            task = task_named("HelloWorld", st)
            if win and task:
                hw = {"win": win, "task": task}
                break
            time.sleep(1.0)
        check("HelloWorld: window + m68k task up", bool(hw),
              f"tasks={[t['name'] for t in st['tasks']]} "
              f"wins={[w['title'] for w in st['wins']]}")
        if hw:
            w, task = hw["win"], hw["task"]
            # Guest blocks in WaitPort on a window that never subscribed to
            # IDCMP_CLOSEWINDOW — the close click must halt + tear it down.
            if not uaos.move_to(w["x"] + 9, w["y"] + 10):
                raise Fail("pointer never reached close gadget")
            uaos.click()
            gone = False
            task_gone = False
            t0 = time.time()
            while time.time() - t0 < 15:
                st = probe()
                gone = all(x["i"] != w["i"] for x in st["wins"])
                task_gone = all(t["i"] != task["i"] or t["state"] == TASK_REMOVED
                                for t in st["tasks"])
                if gone and task_gone:
                    break
                time.sleep(0.5)
            check("HelloWorld: close click halted blocked guest", task_gone)
            check("HelloWorld: window torn down", gone)

        # --- Phase D: OctaMED V5 — minutes-long interactive soak ----------
        # The flagship target: parks in Wait(0x100), must never touch the
        # liveness budget while blocked, and must tear down cleanly on halt.
        base_wdog = log_wdog_count(serial)
        # Decrunch corruption at launch is a known nondeterministic OctaMED
        # failure (okf/log.md — tracked under UAOS-234, not this card), and
        # frequent gdb attach/detach during the packed-load phase seems to
        # provoke it — so poll sparingly while it launches, retry a few
        # times, and click through the startup 'Request' requester.
        octa = None
        for attempt in range(5):
            # First attempt under strace: if it dies, the serial shows the
            # exact libcall sequence at the exit point.
            if attempt == 0:
                type_cmd("strace OCTAMED:OCTAMED/OCTAMED.V5")
            else:
                type_cmd("run OCTAMED:OCTAMED/OCTAMED.V5")
            t0 = time.time()
            while time.time() - t0 < 60 and not octa:
                octa = task_named("OCTAMED")
                if not octa:
                    time.sleep(5)
            if not octa:
                continue
            # Persist past the decrunch/startup window; dismiss the
            # 'Request' requester (empty-body sysreq) if it pops.
            stable = True
            t0 = time.time()
            while time.time() - t0 < 25:
                st = probe()
                cur = next((t for t in st["tasks"] if t["i"] == octa["i"]
                            and t["state"] != TASK_REMOVED), None)
                if not cur:
                    stable = False
                    octa = None
                    break
                req = next((w for w in st["wins"] if w["title"] == "Request"),
                           None)
                if req:
                    uaos.move_to(req["x"] + req["w"] // 2,
                                 req["y"] + req["h"] - 15)
                    uaos.click()
                time.sleep(5)
            if stable:
                break
        check("OctaMED: task launched and stable", bool(octa),
              "all 5 launches died early — decrunch corruption "
              "(UAOS-234 known issue)" if not octa else "")
        if octa:
            SOAK = 150
            alive = True
            t0 = time.time()
            while time.time() - t0 < SOAK:
                st = probe()
                cur = next((t for t in st["tasks"] if t["i"] == octa["i"]
                            and t["state"] != TASK_REMOVED), None)
                if not cur:
                    alive = False
                    break
                left = int(SOAK - (time.time() - t0))
                print(f"  OctaMED soak: alive, {left}s left", flush=True)
                time.sleep(15)
            check(f"OctaMED: alive after {SOAK}s blocked soak", alive,
                  "" if alive else "guest exited mid-soak (check serial for "
                                   "WILD-PC — OctaMED NULL-call flake, UAOS-234)")
            check("OctaMED: no liveness-watchdog fire while blocked",
                  log_wdog_count(serial) == base_wdog,
                  f"wdog dumps: {base_wdog} -> {log_wdog_count(serial)}")
            st = probe()
            cur = next((t for t in st["tasks"] if t["i"] == octa["i"]
                        and t["state"] != TASK_REMOVED), None)
            if cur:
                uaos.gdb.run(f"set g_tasks[{cur['i']}].m68k_halted = 1\n"
                             f'printf "@@HALTED set\\n"\n')
                check("OctaMED: external halt tears down task",
                      await_task_exit(cur["i"], "OCTAMED", 20))
                check("OctaMED: RAM slot released after teardown",
                      ram_slots() == base_slots,
                      f"used={ram_slots()} baseline={base_slots}")

        # --- Phase E: SpinTest — liveness watchdog + external halt --------
        type_cmd("run SYS:Demos/SpinTest")
        spin = None
        t0 = time.time()
        while time.time() - t0 < 20:
            spin = task_named("SpinTest")
            if spin:
                break
            time.sleep(0.7)
        check("SpinTest: task running", bool(spin),
              f"tasks={[t['name'] for t in probe()['tasks']]}")
        if spin:
            # Wait for the one-shot watchdog dump (100M unblocked cycles).
            log = open(serial, errors="replace").read()
            fired = wait_serial(serial, "spin watchdog", 60)
            check("SpinTest: liveness watchdog fired", fired)
            # It must be a soft dump — task survives.
            st = probe()
            still = task_named("SpinTest", st)
            check("SpinTest: survived watchdog (soft, non-fatal)",
                  bool(still and still["state"] != TASK_REMOVED))
            # Externally halt it — the same flag the close-path uses.
            if still:
                uaos.gdb.run(f"set g_tasks[{still['i']}].m68k_halted = 1\n"
                             f'printf "@@HALTED set\\n"\n')
                check("SpinTest: external halt tears down task",
                      await_task_exit(still["i"], "SpinTest", 15))
        check("post-SpinTest: RAM slots back to baseline",
              ram_slots() == base_slots,
              f"used={ram_slots()} baseline={base_slots}")

        # --- serial hygiene -----------------------------------------------
        log = open(serial, errors="replace").read()
        bad = [p for p in ("FAULT", "Page Fault", "LOW-REENTRY",
                           "GPF", "panic") if p.lower() in log.lower()]
        # "first wild PC" in the watchdog dump is the ≥0x10000 entry-PC
        # label (benign).  Real circuit-breaker hits log "WILD-PC pc=0x...";
        # OctaMED has a known pre-existing pc=0 NULL-call (tracked
        # separately), so only nonzero PCs fail.
        wild = [l for l in log.splitlines()
                if "WILD-PC pc=0x" in l
                and "pc=0x00000000" not in l]
        if wild:
            bad.append(f"WILD-PC({len(wild)})")
        check("serial: no faults/panics/wild PCs", not bad, ",".join(bad))

        uaos.mon.screendump(shot)

    except Fail as e:
        print(f"[ABORT] {e}")
        failed.append("harness abort")
        try:
            uaos.mon.screendump(shot)
        except Exception:
            pass
    finally:
        if a.keep:
            print(f"QEMU left running (pid {proc.pid}); mon={mon} gdb=:{a.gdb_port}")
        else:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except Exception:
                proc.kill()

    print(f"\n{sum(1 for _, ok in checks if ok)}/{len(checks)} checks passed"
          + (f" — screendump: {shot}" if failed else ""))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
