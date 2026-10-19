#!/usr/bin/env python3
"""qemu_screen_test.py — custom-screen isolation regression (UAOS-348).

Boots the UAOS ISO headless, launches SYS:Demos/MenuDemo (OpenScreenTagList
CUSTOMSCREEN + WA_CustomScreen window + one-item menu strip) and asserts,
via the GDB stub, Amiga screen semantics:

  * custom screen front: only its window is visible/focused, Shell hidden
  * screen-bar depth gadget cycles to Workbench (Shell back, demo hidden)
    and back again, focus following the visible screen
  * screen pull-down drag (UAOS-349): grabbing the front screen's bar and
    dragging down slides the screen + its window down and reveals the
    screen behind (Workbench: Shell window shown in layer 1, clickable);
    dragging back up covers it again and focus returns to the demo
  * Project -> Quit menu pick exits the task; Workbench returns

Screendumps land in /tmp/sc_*.ppm.
Run: tests/qemu_screen_test.py      Exit 0 = all checks passed.
"""
import os, re, socket, subprocess, sys, tempfile, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL_ELF = os.path.join(REPO, "build", "uaos-kernel.elf")
ISO = os.path.join(REPO, "build", "Ultimate_Amiga_OS.iso")
MON, SERIAL, VARS = "/tmp/uaos_sc_mon", "/tmp/uaos_sc_serial.log", "/tmp/uaos_sc_ovmf.fd"
GDB_PORT = 12401
SHOT = "/tmp/sc_{}.ppm"

class Monitor:
    def __init__(self, path): self.path = path
    def _drain(self, s):
        s.settimeout(0.15)
        try:
            while s.recv(65536): pass
        except socket.timeout: pass
    def send(self, *cmds):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(self.path); self._drain(s)
        for c in cmds:
            s.sendall(c.encode() + b"\n"); time.sleep(0.03)
        self._drain(s); s.close()
    def type_string(self, text):
        K = {c: c for c in "abcdefghijklmnopqrstuvwxyz0123456789"}
        for c in "abcdefghijklmnopqrstuvwxyz": K[c.upper()] = "shift-" + c
        K.update({" ": "spc", ".": "dot", "/": "slash", ":": "shift-semicolon"})
        self.send(*["sendkey " + K[ch] for ch in text])

class Gdb:
    def __init__(self, port): self.port = port
    def run(self, script, timeout=25):
        full = ("set pagination off\nset print elements 0\n"
                f"target remote :{self.port}\n" + script + "\ndetach\nquit\n")
        with tempfile.NamedTemporaryFile("w", suffix=".gdb", delete=False) as f:
            f.write(full); path = f.name
        try:
            out = subprocess.run(["gdb", "-q", "-nx", "-batch", "-x", path, KERNEL_ELF],
                                 cwd=REPO, capture_output=True, text=True, timeout=timeout)
        finally:
            os.unlink(path)
        return [l for l in out.stdout.splitlines() if l.startswith("@@")]
    def mouse(self):
        out = self.run(r'printf "@@M x=%d y=%d w=%d\n", g_mouse.x, g_mouse.y, g_fb.width')
        m = re.search(r"x=(\d+) y=(\d+) w=(\d+)", out[0])
        return int(m.group(1)), int(m.group(2)), int(m.group(3))
    def state(self):
        out = self.run('''printf "@@F focus=%d\\n", g_focus
set $i = 0
while $i < 64
  if g_tasks[$i].type == 1 && g_tasks[$i].m68k_ram != 0
    printf "@@T state=%d name=%s\\n", g_tasks[$i].tc_State, g_tasks[$i].m68k_name
  end
  set $i = $i + 1
end
set $i = 0
while $i < 16
  if g_wins[$i].active
    printf "@@W i=%d t=%s\\n", $i, g_wins[$i].title
  end
  set $i = $i + 1
end''')
        st = {"focus": -1, "tasks": [], "wins": {}}
        for l in out:
            if l.startswith("@@F"): st["focus"] = int(l.split("=")[1])
            elif l.startswith("@@T"):
                m = re.search(r"state=(\d+) name=(.*)", l); st["tasks"].append((int(m.group(1)), m.group(2)))
            elif l.startswith("@@W"):
                m = re.search(r"i=(\d+) t=(.*)", l); st["wins"][int(m.group(1))] = m.group(2)
        return st

for p in (MON, SERIAL):
    try: os.unlink(p)
    except FileNotFoundError: pass
subprocess.run(["cp", "/usr/share/OVMF/OVMF_VARS_4M.fd", VARS], check=True)
proc = subprocess.Popen(["qemu-system-x86_64", "-machine", "q35",
    "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive", f"if=pflash,format=raw,file={VARS}",
    "-device", "piix3-ide,id=ide",
    "-drive", f"if=none,id=cdrom,media=cdrom,file={ISO}",
    "-device", "ide-cd,drive=cdrom,bus=ide.0",
    "-device", "virtio-blk-pci,disable-modern=on,drive=blk0",
    "-drive", f"id=blk0,file={REPO}/build/uaos_disk.qcow2,if=none,format=qcow2",
    "-serial", f"file:{SERIAL}", "-monitor", f"unix:{MON},server,nowait",
    "-gdb", f"tcp::{GDB_PORT}", "-m", "512M", "-vga", "virtio",
    "-display", "none", "-no-reboot", "-no-shutdown"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
mon, gdb = Monitor(MON), Gdb(GDB_PORT)
results = []
def check(name, ok, detail=""):
    results.append(ok); print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""), flush=True)

def move_to(tx, ty):
    for _ in range(60):
        cx, cy, _ = gdb.mouse()
        dx, dy = tx - cx, ty - cy
        if abs(dx) <= 2 and abs(dy) <= 2: return True
        mon.send(f"mouse_move {max(-60,min(60,dx))} {max(-60,min(60,dy))}")
        time.sleep(0.08)
    return False

def click():
    mon.send("mouse_button 1"); time.sleep(0.2); mon.send("mouse_button 0"); time.sleep(0.8)

def shell_win(st):
    return next((i for i, t in st["wins"].items() if "Shell" in t), None)
def demo_win(st):
    return next((i for i, t in st["wins"].items() if "Menu Demo" in t), None)

def visible(i):
    out = gdb.run(f'printf "@@V %d\\n", WM_IsWindowVisible({i})')
    return out and out[0].endswith("1")

def layer(i):
    out = gdb.run(f'printf "@@L %d\\n", WM_WindowLayer({i})')
    return int(out[0].split()[1]) if out else -2

def front_top():
    out = gdb.run('''set $i = 0
while $i < 4
  if g_intu_screens[$i].active && g_intu_screens[$i].is_front
    printf "@@S %d\\n", g_intu_screens[$i].top
  end
  set $i = $i + 1
end''')
    return int(out[0].split()[1]) if out else None

def win_y(i):
    out = gdb.run(f'printf "@@Y %d\\n", g_wins[{i}].y')
    return int(out[0].split()[1]) if out else None

def drag(x, y0, y1):
    move_to(x, y0)
    mon.send("mouse_button 1"); time.sleep(0.2)
    step = 40 if y1 > y0 else -40
    for _ in range(abs(y1 - y0) // 40):
        mon.send(f"mouse_move 0 {step}"); time.sleep(0.06)
    move_to(x, y1)
    time.sleep(0.3)
    mon.send("mouse_button 0"); time.sleep(0.8)

try:
    for _ in range(50):
        if os.path.exists(MON): break
        time.sleep(0.1)
    t0 = time.time()
    st = None
    while time.time() - t0 < 120:
        try:
            st = gdb.state()
            if shell_win(st) is not None: break
        except Exception: pass
        time.sleep(2)
    check("boot: shell up", st and shell_win(st) is not None)
    time.sleep(3)
    mon.send(f"screendump {SHOT.format('0_wb')}")

    mon.type_string("SYS:Demos/MenuDemo"); mon.send("sendkey ret")
    t0 = time.time()
    while time.time() - t0 < 30:
        st = gdb.state()
        if demo_win(st) is not None: break
        time.sleep(1)
    time.sleep(2)
    st = gdb.state()
    sw, dw = shell_win(st), demo_win(st)
    check("launch: MenuDemo window", dw is not None, str(st["wins"]))
    check("custom screen front: demo window visible", visible(dw))
    check("custom screen front: Shell hidden", not visible(sw))
    check("custom screen front: focus on demo window", st["focus"] == dw, f"focus={st['focus']}")
    mon.send(f"screendump {SHOT.format('1_custom')}")

    _, _, W = gdb.mouse()
    # depth gadget: far right of the bar
    move_to(W - 10, 8); click()
    st = gdb.state()
    check("depth gadget -> Workbench: Shell visible", visible(sw))
    check("depth gadget -> Workbench: demo hidden", not visible(dw))
    check("depth gadget -> Workbench: focus left demo", st["focus"] != dw, f"focus={st['focus']}")
    mon.send(f"screendump {SHOT.format('2_wb_again')}")

    move_to(W - 10, 8); click()
    st = gdb.state()
    check("depth gadget -> custom again: demo visible, Shell hidden",
          visible(dw) and not visible(sw))
    check("depth gadget -> custom again: focus back on demo", st["focus"] == dw, f"focus={st['focus']}")
    mon.send(f"screendump {SHOT.format('3_custom_again')}")

    # --- screen pull-down drag (UAOS-349) --------------------------------
    DROP = 300
    demo_y0 = win_y(dw)
    drag(400, 8, 8 + DROP)
    top = front_top()
    check("drag down: front screen top follows the bar", top is not None and abs(top - DROP) <= 6,
          f"top={top}")
    dy = (win_y(dw) or 0) - (demo_y0 or 0)
    check("drag down: demo window rides with its screen", abs(dy - (top or 0)) <= 1,
          f"window dy={dy}")
    check("drag down: Shell revealed in the screen-behind layer", layer(sw) == 1,
          f"layer={layer(sw)}")
    check("drag down: demo window still front layer", layer(dw) == 0)
    mon.send(f"screendump {SHOT.format('3b_dragged')}")

    # menus drop from the moved bar: RMB on "Project", check the dropdown
    # anchors below the bar, then release off the menu (cancel)
    move_to(15, (top or DROP) + 5); mon.send("mouse_button 2"); time.sleep(0.6)
    move_to(30, (top or DROP) + 30)
    out = gdb.run('printf "@@MN %d %d\\n", g_menu_index, g_menu_y')
    mi, my_ = (int(v) for v in out[0].split()[1:3]) if out else (-9, -9)
    check("drag down: menu opens from the moved bar", mi == 0 and my_ == (top or 0) + 20,
          f"menu_index={mi} menu_y={my_}")
    mon.send(f"screendump {SHOT.format('3b2_menu_moved')}")
    move_to(700, 650); mon.send("mouse_button 0"); time.sleep(0.8)
    st = gdb.state()
    check("drag down: cancelled menu leaves MenuDemo running",
          any("MenuDemo" in n and s_ != 3 for s_, n in st["tasks"]))

    # click the revealed part of the Workbench Shell window
    move_to(300, 150); click()
    st = gdb.state()
    check("revealed area: click activates the Workbench Shell", st["focus"] == sw,
          f"focus={st['focus']}")
    mon.send(f"screendump {SHOT.format('3c_shell_active')}")

    # push the screen back up — Workbench covered again, focus returns
    drag(400, (top or DROP) + 8, 4)
    top = front_top()
    st = gdb.state()
    check("drag up: screen back at the top", top == 0, f"top={top}")
    check("drag up: Shell hidden again", not visible(sw))
    check("drag up: focus back on the demo window", st["focus"] == dw, f"focus={st['focus']}")
    mon.send(f"screendump {SHOT.format('3d_back_up')}")

    # menu: RMB on the first title, release over item 0
    move_to(15, 5); mon.send("mouse_button 2"); time.sleep(0.6)
    move_to(30, 30)
    mon.send(f"screendump {SHOT.format('4_menu')}")
    mon.send("mouse_button 0"); time.sleep(2)
    st = gdb.state()
    alive = [n for s, n in st["tasks"] if "MenuDemo" in n and s != 3]
    check("QUIT: MenuDemo exited", not alive, str(st["tasks"]))
    check("after QUIT: Shell visible again", visible(sw))
    mon.send(f"screendump {SHOT.format('5_after')}")
    time.sleep(1)
finally:
    proc.terminate()
sys.exit(0 if all(results) else 1)
