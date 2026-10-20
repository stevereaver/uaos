#!/usr/bin/env python3
"""qemu_octamed_cli_test.py — UAOS-320: OctaMED .med CLI-arg repro.

Boots UAOS with build/octamed.img and launches OctaMED V5 *with a .med
argument* — the exact path that exited immediately on metal:

    run OCTAMED:OCTAMED/OCTAMED.V5 OCTAMED:SONGS/cub.med

Dismisses the startup requester, then polls the guest task list: the
bug reproduces iff the OCTAMED m68k task disappears without an error
requester.  On a healthy path the tracker UI loads the song and stays.

Run: tests/qemu_octamed_cli_test.py [--keep] [--timeout SEC]
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_layout_test import Fail, Gdb, Monitor, Uaos, wait_serial, REPO
from qemu_m68k_lifecycle_test import launch_with_hdf

OCTAMED_HDF = os.path.join(REPO, "build", "octamed.img")
TASK_REMOVED = 3


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--iso", default=os.path.join(REPO, "build",
                                                  "Ultimate_Amiga_OS.iso"))
    ap.add_argument("--gdb-port", type=int, default=11245)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--no-arg", action="store_true",
                    help="control run: launch without the .med arg")
    ap.add_argument("--shot-dir", default="/tmp")
    a = ap.parse_args()

    mon = "/tmp/uaos_ocli_mon"
    serial = "/tmp/uaos_ocli_serial.log"
    vars_fd = "/tmp/uaos_ocli_ovmf.fd"
    checks, failed = [], []

    def check(name, ok, detail=""):
        tag = "PASS" if ok else "FAIL"
        checks.append((name, ok))
        print(f"[{tag}] {name}" + (f" — {detail}" if detail else ""), flush=True)
        if not ok:
            failed.append(name)

    proc = launch_with_hdf(a.iso, a.gdb_port, mon, serial, vars_fd,
                           hdf=OCTAMED_HDF)
    uaos = Uaos(Monitor(mon), Gdb(a.gdb_port))
    shot = os.path.join(a.shot_dir, "uaos_ocli_test.ppm")

    def probe():
        return uaos.gdb.probe_state()

    def octa_task(st):
        return next((t for t in st["tasks"]
                     if "OCTAMED" in t["name"].upper()
                     and t["state"] != TASK_REMOVED), None)

    REQ = "'intuition_lib.c'::g_req_slot"

    def req_state():
        """g_req_slot: (active, wm_handle, btn0 x, btn0 y) or None."""
        out = uaos.gdb.run(
            f'printf "@@R a=%d wh=%d nb=%d bx=%d by=%d\\n",'
            f' {REQ}.active, {REQ}.wm_handle, {REQ}.num_buttons,'
            f' {REQ}.btn_x[0], {REQ}.btn_y[0]')
        m = re.search(r"a=(\d+) wh=(-?\d+) nb=(\d+) bx=(-?\d+) by=(-?\d+)",
                      out[0]) if out else None
        return tuple(int(v) for v in m.groups()) if m else None

    def dismiss_request(st):
        """Click button 0 (OK/Continue) of any live g_req_slot window —
        covers both the plain "Request" (MED_Paths) and the "OctaMED
        Request" (old-format fallback) modals."""
        r = req_state()
        if not r or not r[0] or r[1] < 0:
            return False
        win = next((w for w in st["wins"] if w["i"] == r[1]), None)
        if not win:
            return False
        uaos.move_to(win["x"] + r[3] + 35, win["y"] + r[4] + 10)
        uaos.click()
        time.sleep(2)
        return True

    try:
        check("boot: telnetd up", wait_serial(serial, "telnetd: listening", 120))

        if a.no_arg:
            # control run: plain launch, no .med arg
            uaos.mon.type_string("run OCTAMED:OCTAMED/OCTAMED.V5")
        else:
            # CLI-arg path from the card.
            uaos.mon.type_string(
                "run OCTAMED:OCTAMED/OCTAMED.V5 OCTAMED:SONGS/cub.med")
        uaos.mon.send("sendkey ret")

        # OctaMED's startup requester is modal — keep dismissing it while
        # watching for the task; on metal the task vanished here.
        octa = None
        t0 = time.time()
        while time.time() - t0 < 90:
            st = probe()
            if not octa:
                octa = octa_task(st)
            dismiss_request(st)
            if octa and not octa_task(st):
                break
            time.sleep(4)
        st = probe()
        octa = octa_task(st)
        check("OctaMED: task survives .med CLI arg", bool(octa))

        # Give it a while to finish loading cub.med after the last
        # requester dismissal, then re-check.
        if octa:
            t0 = time.time()
            while time.time() - t0 < 60:
                st = probe()
                if dismiss_request(st):
                    t0 = time.time()          # late requesters
                    continue
                if not octa_task(st):
                    break
                time.sleep(4)
            octa = octa_task(probe())
            check("OctaMED: still alive after song-load settle", bool(octa))

        # CLI-arg evidence: OctaMED V5 does not auto-open a positional
        # filename argument (Workbench-app arg semantics — verified live:
        # no Open() call after the arg lands in argv).  Assert delivery
        # instead: m68k_argv[1] must carry the .med path.
        if octa:
            out = uaos.gdb.run(
                'printf "@@ARGV i=%d a=%s\\n",'
                f' {octa["i"]}, g_tasks[{octa["i"]}].m68k_argv[1]')
            arg = out[0] if out else ""
            if a.no_arg:
                check("CLI arg: argv[1] empty for --no-arg control",
                      "cub.med" not in arg.lower(), arg.strip())
            else:
                check("CLI arg: .med path in m68k_argv[1]",
                      "cub.med" in arg.lower(), arg.strip())
        else:
            check("CLI arg: .med path in m68k_argv[1]", False,
                  "task gone")

        # --- ASL path (the card's primary repro): Shift+O -> requester --
        if not octa:
            raise Fail("OctaMED task gone before ASL phase")
        # OctaMED pops late modal "Request" windows — clear them all
        # before focusing, or the WM swallows the click+keypress.
        t0 = time.time()
        while time.time() - t0 < 20:
            if dismiss_request(probe()):
                t0 = time.time()
                continue
            break
        time.sleep(2)
        while dismiss_request(probe()):
            time.sleep(1)
        # focus the tracker window (largest non-shell window), then
        # Shift+O = module load in OctaMED.
        st = probe()
        octa_win = None
        for w in st["wins"]:
            if "Shell" in w["title"] or w["title"] == "Request":
                continue
            if octa_win is None or w["w"] * w["h"] > octa_win["w"] * octa_win["h"]:
                octa_win = w
        # the 60s blanker eats the first input event as "unblank" — burn
        # one click before the real focus click.
        uaos.move_to(320, 200)
        uaos.click()
        time.sleep(1)
        if octa_win:
            # Shell 1 covers the window's centre — click the exposed
            # left edge sliver (window x..shell x) to activate it.
            shell = next((w for w in st["wins"]
                          if "Shell" in w["title"]), None)
            cx = octa_win["x"] + octa_win["w"] // 2
            cy = octa_win["y"] + octa_win["h"] // 2
            if shell and (shell["x"] <= cx < shell["x"] + shell["w"]
                          and shell["y"] <= cy < shell["y"] + shell["h"]):
                cx = octa_win["x"] + 10   # left sliver outside the shell
            uaos.move_to(cx, cy)
        else:
            uaos.move_to(160, 25)
        uaos.click()
        time.sleep(1)

        FRQ = "'intuition_lib.c'::g_frq"

        def frq_geom(field):
            out = uaos.gdb.run(f'printf "@@G %d\\n", {FRQ}.{field}')
            m = re.search(r"@@G (-?\d+)", out[0]) if out else None
            return int(m.group(1)) if m else 0

        def frq_open():
            """ASL requester state: active && !done."""
            out = uaos.gdb.run(
                f'printf "@@FRQ a=%d d=%d wh=%d\\n",'
                f' {FRQ}.active, {FRQ}.done, {FRQ}.wm_handle')
            m = re.search(r"a=(\d+) d=(\d+) wh=(-?\d+)", out[0]) if out else None
            return (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m \
                else (0, 0, -1)

        def frq_win():
            a, d, wh = frq_open()
            if not a or d:
                return None
            return next((w for w in probe()["wins"] if w["i"] == wh), None)

        def frq_names():
            out = uaos.gdb.run(
                f'set $i = 0\n'
                f'while $i < {FRQ}.nent\n'
                f'  printf "@@N %d %s\\n", $i, {FRQ}.ents[$i].name\n'
                f'  set $i = $i + 1\n'
                f'end\n')
            res = []
            for l in out:
                if not l.startswith("@@N "):
                    continue
                sp = l.index(" ", 4)
                res.append((int(l[4:sp]), l[sp + 1:]))
            return res

        def frq_btn(win, idx):
            return (win["x"] + frq_geom(f"btn_x[{idx}]") + 38,
                    win["y"] + frq_geom("btn_y") + 9)

        def frq_scroll(win, direction):
            """One click on the ^/v strip at the list's right edge."""
            rows = frq_geom("rows")
            ly = win["y"] + frq_geom("list_y")
            sx = win["x"] + win["w"] - 18 - 20 + 8     # ss_left + half strip
            mid = ly + rows * 7                      # rows*14/2
            uaos.move_to(sx, mid - 7 if direction < 0 else mid + 7)
            uaos.click()
            time.sleep(1)

        def frq_click_row(win, pred):
            """Single click the row matching pred (dirs descend on one
            click; files select).  Scrolls the list until the row is
            visible.  Returns True if a row was clicked."""
            want = None
            for i, n in frq_names():
                if pred(n):
                    want = i
                    break
            if want is None:
                return False
            for _ in range(20):
                top = frq_geom("top")
                rows = frq_geom("rows")
                row = want - top
                if 0 <= row < rows:
                    y = win["y"] + frq_geom("list_y") + row * 14 + 7
                    uaos.move_to(win["x"] + 60, y)
                    uaos.click()
                    time.sleep(2)
                    return True
                frq_scroll(win, -1 if row < 0 else 1)
            return False

        def await_frq(timeout=20):
            t0 = time.time()
            while time.time() - t0 < timeout:
                w = frq_win()
                if w:
                    return w
                # OctaMED pops an intermediate one-button "Request"
                # before calling AslRequest — dismiss it or the flow
                # stalls.
                dismiss_request(probe())
                time.sleep(1)
            return None

        # mechanics check: a plain RAWKEY must reach OctaMED's window —
        # its IDCMP subscribes RAWKEY and the post is logged on serial.
        n_raw = len(re.findall(r"\[rawkey\]",
                               open(serial, errors="replace").read()))
        uaos.mon.send("sendkey a")
        time.sleep(3)
        log = open(serial, errors="replace").read()
        n_raw2 = len(re.findall(r"\[rawkey\]", log))
        check("mechanics: RAWKEY reaches focused window", n_raw2 > n_raw,
              f"[rawkey] lines {n_raw} -> {n_raw2}")

        # The card's real path: Project -> Open... is menu command-key
        # O — RAmiga+O (QEMU sends the Right-GUI key as 'meta_r'), which
        # the WM turns into a MENUPICK for the focused window's menu.
        uaos.mon.send("sendkey meta_r-o")
        time.sleep(3)
        asl = await_frq(30)
        check("ASL: module-load requester opened", bool(asl))

        if asl:
            # Volumes -> OCTAMED: -> Songs -> cub.med -> OK
            uaos.move_to(*frq_btn(asl, 0)); uaos.click(); time.sleep(2)
            ok1 = frq_click_row(asl, lambda n: n.rstrip("/") == "OCTAMED:")
            check("ASL: OCTAMED: volume entered", ok1,
                  f"ents={[n for _, n in frq_names()][:8]}")
            ok2 = frq_click_row(asl, lambda n:
                                n.rstrip("/").lower() == "songs")
            check("ASL: Songs dir entered", ok2,
                  f"ents={[n for _, n in frq_names()][:8]}")
            got = frq_click_row(asl, lambda n: "cub" in n.lower()
                                or ".med" in n.lower())
            check("ASL: cub.med visible+selected", got,
                  f"ents={[n for _, n in frq_names()][:8]}")
            uaos.move_to(*frq_btn(asl, 2)); uaos.click(); time.sleep(6)

            # the card's failure: the task exits after the OK click
            t0 = time.time()
            dead = False
            while time.time() - t0 < 30:
                st = probe()
                if dismiss_request(st):
                    continue
                if not octa_task(st):
                    dead = True
                    break
                time.sleep(3)
            check("OctaMED: survives ASL song load", not dead)
            log = open(serial, errors="replace").read()
            check("serial: cub.med opened via ASL",
                  "cub.med" in log.lower() or "cub~1" in log.lower())

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
            print(f"QEMU left running (pid {proc.pid}); mon={mon}")
        else:
            proc.terminate()

    print(f"\n{sum(1 for _, ok in checks if ok)}/{len(checks)} checks passed"
          + (f" — screendump: {shot}" if failed else ""))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
