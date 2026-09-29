#!/usr/bin/env python3
"""qemu_layout_test.py — scripted QEMU regression test for the M68k
BOOPSI layout.gadget path (UAOS-128).

Boots the UAOS ISO headless, launches SYS:Demos/LayoutTest through the
console shell, drives the PS/2 pointer through the QEMU human monitor
with closed-loop positioning (reads the kernel's g_mouse via the GDB
stub — PS/2 deltas are relative, so we measure instead of guessing),
and asserts on kernel + guest state read through the same stub:

  * gadget list:  IDs 101-106 present, nested hgroup row laid out
  * clicks:       East/West/checkbox update the WM title via IDCMP
                  GADGETUP -> GadgetID -> SetWindowTitles
  * resize:       sizing-gadget drag reflows the layout live
  * quit:         Quit button exits the task cleanly
  * serial:       no cycle-budget aborts / wild PCs / faults

Requires: gdb (with the kernel ELF's DWARF info), QEMU, a built ISO.
Run:  scripts/qemu_layout_test.py [--iso PATH] [--keep] [--timeout SEC]

Exit 0 = all checks passed.  On failure the last screendump is kept
(--shot-dir, default $TMPDIR) for post-mortem inspection.
"""

import argparse
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL_ELF = os.path.join(REPO, "build", "uaos-kernel.elf")

# ---------------------------------------------------------------------------
# Guest structure offsets (big-endian M68k layout — see intuition_lib.h)
# ---------------------------------------------------------------------------
WIN_LEFT, WIN_TOP, WIN_W, WIN_H = 4, 6, 8, 10
WIN_FIRSTGADGET, WIN_USERPORT, WIN_IDCMP = 62, 86, 82
GAD_NEXT, GAD_LEFT, GAD_TOP = 0, 4, 6
GAD_W, GAD_H, GAD_FLAGS, GAD_ID = 8, 10, 12, 38
GFLG_SELECTED = 0x0040

TASK_REMOVED, TASK_TYPE_M68K = 3, 1

KEYMAP = {}
for _c in "abcdefghijklmnopqrstuvwxyz0123456789":
    KEYMAP[_c] = _c
for _c in "abcdefghijklmnopqrstuvwxyz":
    KEYMAP[_c.upper()] = "shift-" + _c
KEYMAP.update({
    " ": "spc", ".": "dot", "/": "slash", "-": "minus",
    ":": "shift-semicolon", ">": "shift-dot", "<": "shift-comma",
    '"': "shift-apostrophe", "_": "shift-minus", "=": "equal",
    ";": "semicolon", "{": "shift-bracket_left", "}": "shift-bracket_right",
    "(": "shift-9", ")": "shift-0", "*": "shift-8", "#": "shift-3",
    "%": "shift-5", "'": "apostrophe", ",": "comma", "!": "shift-1",
})


class Fail(Exception):
    pass


# ---------------------------------------------------------------------------
# QEMU human monitor over a unix socket
# ---------------------------------------------------------------------------
class Monitor:
    def __init__(self, path):
        self.path = path

    def _drain(self, s):
        s.settimeout(0.15)
        try:
            while s.recv(65536):
                pass
        except socket.timeout:
            pass

    def send(self, *cmds):
        """Send monitor commands; each returns after its (qemu) prompt."""
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(self.path)
        self._drain(s)
        for c in cmds:
            s.sendall(c.encode() + b"\n")
            time.sleep(0.03)
        self._drain(s)
        s.close()

    def type_string(self, text, key_delay=0.025):
        keys = []
        for ch in text:
            k = KEYMAP.get(ch)
            if k is None:
                raise Fail(f"no sendkey mapping for {ch!r}")
            keys.append("sendkey " + k)
        self.send(*keys)

    def screendump(self, path):
        self.send(f'screendump {path}')


# ---------------------------------------------------------------------------
# GDB batch probes (QEMU stub).  Each call halts the VM, runs the script,
# detaches.  Scripts print @@KEY=VALUE lines which are parsed into a dict.
# ---------------------------------------------------------------------------
class Gdb:
    def __init__(self, port):
        self.port = port

    def run(self, script, timeout=25):
        """Run a gdb script; return list of @@-marked output lines.
        The script goes through a file (-x) rather than -ex so multi-line
        blocks (while/end, if/end) parse correctly."""
        full = ("set pagination off\nset print elements 0\n"
                f"target remote :{self.port}\n"
                + script + "\ndetach\nquit\n")
        with tempfile.NamedTemporaryFile("w", suffix=".gdb",
                                         delete=False) as f:
            f.write(full)
            path = f.name
        try:
            out = subprocess.run(
                ["gdb", "-q", "-nx", "-batch", "-x", path, KERNEL_ELF],
                cwd=REPO, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise Fail("gdb probe timed out (VM wedged?)")
        finally:
            os.unlink(path)
        return [l for l in out.stdout.splitlines() if l.startswith("@@")]

    def probe_mouse(self):
        out = self.run(r'''
printf "@@MOUSE x=%d y=%d bl=%d br=%d\n", g_mouse.x, g_mouse.y, g_mouse.btn_left, g_mouse.btn_right
''')
        m = re.search(r"x=(\d+) y=(\d+) bl=(\d+) br=(\d+)", out[0])
        return tuple(int(g) for g in m.groups())

    def probe_state(self):
        """One attach: tasks + windows + intuition slots + mouse."""
        script = ['printf "@@MOUSE x=%d y=%d bl=%d\\n", g_mouse.x, g_mouse.y, g_mouse.btn_left']
        script += ['set $i = 0',
                   'while $i < 64',
                   '  if g_tasks[$i].type == 1 && g_tasks[$i].m68k_ram != 0',
                   '    printf "@@TASK i=%d ram=%p state=%d name=%s\\n", $i, g_tasks[$i].m68k_ram, g_tasks[$i].tc_State, g_tasks[$i].m68k_name',
                   '  end',
                   '  set $i = $i + 1',
                   'end']
        script += ['set $i = 0',
                   'while $i < 16',
                   '  if g_wins[$i].active',
                   '    printf "@@WIN i=%d x=%d y=%d w=%d h=%d t=%s\\n", $i, g_wins[$i].x, g_wins[$i].y, g_wins[$i].w, g_wins[$i].h, g_wins[$i].title',
                   '  end',
                   '  set $i = $i + 1',
                   'end']
        script += ['set $i = 0',
                   'while $i < 8',
                   '  if g_intu_wins[$i].active',
                   '    printf "@@SLOT i=%d h=%d gw=0x%x size=0x%x\\n", $i, g_intu_wins[$i].wm_handle, g_intu_wins[$i].guest_win, g_intu_wins[$i].gad_size',
                   '  end',
                   '  set $i = $i + 1',
                   'end']
        out = self.run("\n".join(script))
        tasks, wins, slots, mouse = [], [], [], None
        for l in out:
            if l.startswith("@@MOUSE"):
                m = re.search(r"x=(\d+) y=(\d+)", l)
                mouse = (int(m.group(1)), int(m.group(2)))
            elif l.startswith("@@TASK"):
                m = re.search(r"i=(\d+) ram=(0x[0-9a-f]+) state=(\d+) name=(.*)", l)
                tasks.append({"i": int(m.group(1)), "ram": int(m.group(2), 16),
                              "state": int(m.group(3)), "name": m.group(4)})
            elif l.startswith("@@WIN"):
                m = re.search(r"i=(\d+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) t=(.*)", l)
                wins.append({"i": int(m.group(1)), "x": int(m.group(2)),
                             "y": int(m.group(3)), "w": int(m.group(4)),
                             "h": int(m.group(5)), "title": m.group(6)})
            elif l.startswith("@@SLOT"):
                m = re.search(r"i=(\d+) h=(\d+) gw=(0x[0-9a-f]+) size=(0x[0-9a-f]+)", l)
                slots.append({"handle": int(m.group(2)),
                              "guest_win": int(m.group(3), 16),
                              "gad_size": int(m.group(4), 16)})
        return {"tasks": tasks, "wins": wins, "slots": slots, "mouse": mouse}

    def probe_gadgets(self, ram, win_ptr):
        """Walk the guest gadget list (big-endian).  Returns list of dicts.
        Byte-array indexing is used for BE decode — gdb 'define' args get
        mis-parsed when unparenthesized, so no helper functions."""
        script = f'''
set $ram = (unsigned char*){ram}
set $base = $ram + {win_ptr}
printf "@@GW w=%d h=%d first=0x%x\\n", $base[{WIN_W}]*256+$base[{WIN_W}+1], $base[{WIN_H}]*256+$base[{WIN_H}+1], $base[{WIN_FIRSTGADGET}]*16777216+$base[{WIN_FIRSTGADGET}+1]*65536+$base[{WIN_FIRSTGADGET}+2]*256+$base[{WIN_FIRSTGADGET}+3]
set $gad = $base[{WIN_FIRSTGADGET}]*16777216 + $base[{WIN_FIRSTGADGET}+1]*65536 + $base[{WIN_FIRSTGADGET}+2]*256 + $base[{WIN_FIRSTGADGET}+3]
set $n = 0
while $gad != 0 && $n < 32
  set $q = $ram + $gad
  set $nx  = $q[0]*16777216 + $q[1]*65536 + $q[2]*256 + $q[3]
  set $gl  = $q[{GAD_LEFT}]*256 + $q[{GAD_LEFT}+1]
  set $gt  = $q[{GAD_TOP}]*256 + $q[{GAD_TOP}+1]
  set $gw  = $q[{GAD_W}]*256 + $q[{GAD_W}+1]
  set $gh  = $q[{GAD_H}]*256 + $q[{GAD_H}+1]
  set $gf  = $q[{GAD_FLAGS}]*256 + $q[{GAD_FLAGS}+1]
  set $gid = $q[{GAD_ID}]*256 + $q[{GAD_ID}+1]
  printf "@@GAD p=0x%x l=%d t=%d w=%d h=%d f=0x%x id=%d n=0x%x\\n", $gad, $gl, $gt, $gw, $gh, $gf, $gid, $nx
  set $gad = $nx
  set $n = $n + 1
end
'''
        gads = []
        for l in self.run(script):
            m = re.search(r"p=(0x[0-9a-f]+) l=(-?\d+) t=(-?\d+) w=(-?\d+) h=(-?\d+) f=(0x[0-9a-f]+) id=(-?\d+) n=(0x[0-9a-f]+)", l)
            if m:
                gads.append({"ptr": int(m.group(1), 16),
                             "l": int(m.group(2)), "t": int(m.group(3)),
                             "w": int(m.group(4)), "h": int(m.group(5)),
                             "flags": int(m.group(6), 16),
                             "id": int(m.group(7))})
        return gads


# ---------------------------------------------------------------------------
# QEMU lifecycle + closed-loop pointer control
# ---------------------------------------------------------------------------
def launch(iso, port, mon, serial, vars_fd):
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
            "-device", "virtio-net-pci,netdev=n0,disable-modern=on",
            "-serial", f"file:{serial}",
            "-monitor", f"unix:{mon},server,nowait",
            "-gdb", f"tcp::{port}",
            "-m", "512M", "-vga", "virtio", "-display", "none",
            "-no-reboot", "-no-shutdown"]
    proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    # Fail fast if QEMU exits immediately (e.g. gdb port already bound by a
    # stale instance) or never creates the monitor socket.
    for _ in range(50):
        if proc.poll() is not None:
            raise Fail(f"qemu exited at launch (rc={proc.returncode}) — "
                       f"port {port} or {mon} already in use?")
        if os.path.exists(mon):
            return proc
        time.sleep(0.1)
    proc.terminate()
    raise Fail(f"qemu never created monitor socket {mon}")


def wait_serial(path, needle, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            if needle in open(path, errors="replace").read():
                return True
        except FileNotFoundError:
            pass
        time.sleep(0.5)
    return False


class Uaos:
    """High-level driver: monitor + gdb + closed-loop pointer."""
    def __init__(self, mon, gdb):
        self.mon, self.gdb = mon, gdb

    def mouse_pos(self):
        x, y, _, _ = self.gdb.probe_mouse()
        return x, y

    def move_to(self, tx, ty, tol=3, max_iter=60):
        """Step the pointer to (tx,ty); PS/2 deltas are relative so we
        re-measure each step.  A move can get eaten while the button is
        held at screen edges, hence the retry loop."""
        for _ in range(max_iter):
            try:
                cx, cy = self.mouse_pos()
            except Fail:
                continue
            dx, dy = tx - cx, ty - cy
            if abs(dx) <= tol and abs(dy) <= tol:
                return True
            sx = max(-60, min(60, dx))
            sy = max(-60, min(60, dy))
            self.mon.send(f"mouse_move {sx} {sy}")
            time.sleep(0.08)
        return False

    def click(self, settle=0.18):
        self.mon.send("mouse_button 1")
        time.sleep(settle)
        self.mon.send("mouse_button 0")
        time.sleep(0.1)

    def press(self):
        self.mon.send("mouse_button 1")
        time.sleep(0.15)

    def release(self):
        self.mon.send("mouse_button 0")
        time.sleep(0.15)


# ---------------------------------------------------------------------------
# Test flow
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--iso", default=os.path.join(REPO, "build", "Ultimate_Amiga_OS.iso"))
    ap.add_argument("--gdb-port", type=int, default=11234)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--keep", action="store_true", help="leave QEMU running")
    ap.add_argument("--shot-dir", default=tempfile.gettempdir())
    a = ap.parse_args()

    mon = "/tmp/uaos_lttest_mon"
    serial = "/tmp/uaos_lttest_serial.log"
    vars_fd = "/tmp/uaos_lttest_ovmf.fd"
    checks, failed = [], []

    def check(name, ok, detail=""):
        tag = "PASS" if ok else "FAIL"
        checks.append((name, ok))
        print(f"[{tag}] {name}" + (f" — {detail}" if detail else ""))
        if not ok:
            failed.append(name)

    if not os.path.isfile(a.iso):
        sys.exit(f"ISO not found: {a.iso} — run scripts/build_iso.sh")
    if not os.path.isfile(KERNEL_ELF):
        sys.exit(f"kernel ELF not found: {KERNEL_ELF}")

    proc = launch(a.iso, a.gdb_port, mon, serial, vars_fd)
    deadline = time.time() + a.timeout
    uaos = Uaos(Monitor(mon), Gdb(a.gdb_port))
    shot = os.path.join(a.shot_dir, "uaos_layout_test.ppm")

    def deadline_left():
        return max(0.0, deadline - time.time())

    try:
        # --- boot ---------------------------------------------------------
        ok = wait_serial(serial, "telnetd: listening", 120)
        check("boot: serial reaches telnetd", ok)

        st = None
        t0 = time.time()
        while time.time() - t0 < 40:
            try:
                st = uaos.gdb.probe_state()
            except Fail:
                pass
            if st and any("Shell" in w["title"] for w in st["wins"]):
                break
            time.sleep(1.5)
        check("boot: Shell window registered", bool(st and any("Shell" in w["title"] for w in st["wins"])),
              f"wins={len(st['wins']) if st else 0}")
        if not st:
            raise Fail("no state probe ever succeeded")

        # --- launch LayoutTest -------------------------------------------
        uaos.mon.type_string("SYS:Demos/LayoutTest")
        uaos.mon.send("sendkey ret")

        lt = None   # {"win": w, "slot": s, "task": t}
        t0 = time.time()
        while time.time() - t0 < 30:
            st = uaos.gdb.probe_state()
            win = next((w for w in st["wins"] if "Layout" in w["title"]), None)
            slot = next((s for s in st["slots"] if win and s["handle"] == win["i"]), None)
            task = next((t for t in st["tasks"] if "Layout" in t["name"]), None)
            if win and slot and task:
                lt = {"win": win, "slot": slot, "task": task}
                break
            time.sleep(1.0)
        check("launch: LayoutTest window + m68k task", bool(lt),
              f"tasks={[t['name'] for t in st['tasks']]} wins={[w['title'] for w in st['wins']]}")
        if not lt:
            raise Fail("LayoutTest never opened")

        w, slot, task = lt["win"], lt["slot"], lt["task"]
        ram = task["ram"]

        # --- gadget list ---------------------------------------------------
        gads = uaos.gdb.probe_gadgets(ram, slot["guest_win"])
        ids = {g["id"] for g in gads}
        check("gadgets: all demo children spliced (101-106)",
              {101, 102, 103, 104, 105, 106} <= ids,
              f"ids={sorted(ids)}")

        by_id = {g["id"]: g for g in gads}
        row = [by_id.get(i) for i in (101, 102, 103)]
        if all(row):
            same_row = (row[0]["t"] == row[1]["t"] == row[2]["t"]
                        and row[0]["h"] == row[1]["h"] == row[2]["h"])
            ordered = row[0]["l"] < row[1]["l"] < row[2]["l"]
            nonzero = all(g["w"] > 0 and g["h"] > 0 for g in row)
            check("gadgets: hgroup row laid out (same row, ordered, sized)",
                  same_row and ordered and nonzero,
                  " ".join(f"id{g['id']}@{g['l']},{g['t']} {g['w']}x{g['h']}" for g in row))
        else:
            check("gadgets: hgroup row laid out", False, "missing ids")

        # --- raise window: click title bar ---------------------------------
        uaos.move_to(w["x"] + w["w"] // 2, w["y"] + 10)
        uaos.click()

        def click_gadget(gid):
            g = by_id[gid]
            tx = w["x"] + g["l"] + g["w"] // 2
            ty = w["y"] + g["t"] + g["h"] // 2
            if not uaos.move_to(tx, ty):
                raise Fail(f"pointer never reached {tx},{ty}")
            uaos.click()

        def title_now():
            s = uaos.gdb.probe_state()
            ww = next((x for x in s["wins"] if x["i"] == w["i"]), None)
            return ww["title"] if ww else ""

        def click_and_await_title(gid, needle):
            """Click a gadget; poll the WM title for `needle`.  One retry —
            input injection can race window raising on the first click."""
            last_title = ""
            for attempt in range(2):
                click_gadget(gid)
                for _ in range(15):
                    last_title = title_now()
                    if needle in last_title:
                        return True, last_title
                    time.sleep(0.4)
            return False, last_title

        # --- East button ---------------------------------------------------
        ok, t = click_and_await_title(102, "EAST")
        check("click East: title 'Pressed: EAST'", ok, t)

        # --- West button ---------------------------------------------------
        ok, t = click_and_await_title(103, "WEST")
        check("click West: title 'Pressed: WEST'", ok, t)

        # --- checkbox ------------------------------------------------------
        gads = uaos.gdb.probe_gadgets(ram, slot["guest_win"])
        by_id = {g["id"]: g for g in gads}
        chk_before = bool(by_id.get(105, {"flags": 0})["flags"] & GFLG_SELECTED)
        t_ok, t = click_and_await_title(105, "CHECK")
        gads = uaos.gdb.probe_gadgets(ram, slot["guest_win"])
        chk = next((g for g in gads if g["id"] == 105), {"flags": 0})
        f_ok = bool(chk["flags"] & GFLG_SELECTED) != chk_before
        check("click checkbox: title 'Pressed: CHECK'", t_ok, t)
        check("checkbox: GFLG_SELECTED toggled", f_ok,
              f"flags 0x{chk['flags']:x} (was selected={chk_before})")

        # --- resize via sizing gadget --------------------------------------
        gads = uaos.gdb.probe_gadgets(ram, slot["guest_win"])
        east_w_before = next(g for g in gads if g["id"] == 102)["w"]
        st = uaos.gdb.probe_state()
        w2 = next(x for x in st["wins"] if x["i"] == w["i"])
        s2 = next(x for x in st["slots"] if x["handle"] == w["i"])
        # sizing gadget = slot.gad_size guest gadget, else bottom-right corner
        if s2["gad_size"]:
            sg = next((g for g in gads if g["ptr"] == s2["gad_size"]), None)
            sx = w2["x"] + (sg["l"] + sg["w"] // 2) if sg else w2["x"] + w2["w"] - 8
            sy = w2["y"] + (sg["t"] + sg["h"] // 2) if sg else w2["y"] + w2["h"] - 8
        else:
            sx, sy = w2["x"] + w2["w"] - 8, w2["y"] + w2["h"] - 8
        ok = uaos.move_to(sx, sy)
        uaos.press()
        # drag down-right in steps, re-measuring along the way
        for step in range(5):
            uaos.move_to(min(sx + 15 * (step + 1), 990), min(sy + 9 * (step + 1), 750))
        uaos.release()
        time.sleep(0.5)
        st = uaos.gdb.probe_state()
        w3 = next((x for x in st["wins"] if x["i"] == w["i"]), None)
        grew = w3 and w3["w"] > w2["w"] and w3["h"] > w2["h"]
        gads = uaos.gdb.probe_gadgets(ram, slot["guest_win"])
        east = next((g for g in gads if g["id"] == 102), None)
        reflowed = east and east["w"] > east_w_before
        check("resize: window grew + layout reflowed",
              bool(grew and reflowed),
              f"win {w2['w']}x{w2['h']}→{w3 and (w3['w'],w3['h'])} east_w {east_w_before}→{east and east['w']}")

        # --- quit ----------------------------------------------------------
        st = uaos.gdb.probe_state()
        w3 = next(x for x in st["wins"] if x["i"] == w["i"])
        gads = uaos.gdb.probe_gadgets(ram, slot["guest_win"])
        quitg = next(g for g in gads if g["id"] == 106)
        uaos.move_to(w3["x"] + quitg["l"] + quitg["w"] // 2,
                     w3["y"] + quitg["t"] + quitg["h"] // 2)
        uaos.click()
        gone, task_gone = False, False
        for _ in range(20):
            st = uaos.gdb.probe_state()
            gone = all(x["i"] != w["i"] for x in st["wins"])
            task_gone = all(t["i"] != task["i"] or t["state"] == TASK_REMOVED
                            for t in st["tasks"])
            if gone and task_gone:
                break
            time.sleep(0.5)
        check("quit: window closed + task exited", gone and task_gone,
              f"gone={gone} task_gone={task_gone}")

        # --- serial hygiene -------------------------------------------------
        log = open(serial, errors="replace").read()
        bad = [p for p in ("cycle budget", "wild PC", "FAULT", "Page Fault",
                           "exception", "GPF", "panic") if p.lower() in log.lower()]
        check("serial: no aborts/faults", not bad, ",".join(bad))

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
            except subprocess.TimeoutExpired:
                proc.kill()

    print(f"\n{sum(1 for _, ok in checks if ok)}/{len(checks)} checks passed"
          + (f" — screendump: {shot}" if failed else ""))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
