"""gdb_uaos.py — GDB Python helpers for the UAOS kernel (UAOS-213)

Load inside a QEMU GDB-stub session:

    (gdb) target remote :1234
    (gdb) source tools/gdb_uaos.py
    (gdb) uaos tasks
    (gdb) uaos task Workbench
    (gdb) uaos timers
    (gdb) uaos stack Workbench

Requires the kernel ELF to be loaded with symbols (add-symbol-file or
file build/uaos-kernel.elf).  Struct layouts are read from DWARF — the
helpers walk g_tasks[] via task.h types, so they track field renames.
"""

import gdb


def _u64(x):
    return int(x) & 0xFFFFFFFFFFFFFFFF


def _tasks():
    try:
        count = int(gdb.parse_and_eval("g_task_count"))
        arr = gdb.parse_and_eval("g_tasks")
    except gdb.error as e:
        print("uaos: kernel symbols not loaded:", e)
        return []
    out = []
    for i in range(count):
        t = arr[i]
        state = int(t["tc_State"])
        if state == 3:  # TASK_REMOVED
            continue
        out.append((i, t))
    return out


def _name(t):
    try:
        return t["ln_Name"].string()
    except Exception:
        return "?"


_STATES = {0: "wait", 1: "rdy", 2: "run", 3: "gone"}


def _dump_task(idx, t, full=False):
    state = _STATES.get(int(t["tc_State"]), "?")
    line = (f"#{idx:2d} {_name(t):20s} {state:7s} pri={int(t['ln_Pri']):4d} "
            f"rsp=0x{_u64(t['native_rsp']):x} rip=0x{_u64(t['native_rip']):x} "
            f"cpu={_u64(t['cpu_ticks'])}t sw={int(t['ctx_switches'])}")
    if int(t["tc_SigWait"]):
        line += f" wait=0x{int(t['tc_SigWait']):x}"
    print(line)
    if full:
        base = _u64(t["native_stack_base"])
        size = _u64(t["native_stack_size"])
        print(f"    stack=[0x{base:x}..0x{base + size:x}]"
              f" canary={'DEAD' if int(t['stack_overflowed']) else 'ok'}")
        # decode saved frame parked at native_rsp (isr_common layout)
        rsp = _u64(t["native_rsp"])
        try:
            inf = gdb.selected_inferior()
            regs = []
            for i in range(15):
                regs.append(_u64(int.from_bytes(
                    inf.read_memory(rsp + i * 8, 8), "little")))
            rip = _u64(int.from_bytes(inf.read_memory(rsp + 17 * 8, 8), "little"))
            names = ["r15", "r14", "r13", "r12", "r11", "r10", "r9", "r8",
                     "rbp", "rdi", "rsi", "rdx", "rcx", "rbx", "rax"]
            print("    rip=0x%x" % rip, gdb.execute(
                f"info symbol 0x{rip:x}", to_string=True).strip())
            for j in range(0, 15, 5):
                print("    " + "  ".join(f"{names[k]}=0x{regs[k]:x}"
                                         for k in range(j, j + 5)))
        except gdb.error as e:
            print("    <frame read failed>", e)


def _dump_timers():
    try:
        head = gdb.parse_and_eval("g_timer_queue_head")
    except gdb.error:
        print("uaos: no timer queue symbol")
        return
    node = head
    n = 0
    while int(node) != 0 and n < 64:
        tr = node.dereference()
        try:
            fire = _u64(tr["tr_fire_tick"])
        except gdb.error:
            try:
                tv = tr["tr_TimeVal"] if "tr_TimeVal" in tr.type.fields() else None
            except Exception:
                tv = None
            fire = int(tv["tv_Secs"]) * 100 if tv else -1
        print(f"  tr@0x{_u64(node):x} fire_tick={fire}")
        node = tr["tr_Node"]["ln_Succ"] if "tr_Node" in tr.type.fields() else \
               tr["ln_Succ"] if "ln_Succ" in tr.type.fields() else gdb.Value(0)
        n += 1


class UaosCmd(gdb.Command):
    """UAOS kernel inspection.  Usage: uaos tasks|task NAME|timers|stack NAME"""

    def __init__(self):
        super().__init__("uaos", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        args = gdb.string_to_argv(arg)
        if not args or args[0] in ("help",):
            print("uaos tasks            list live tasks")
            print("uaos task NAME        saved-frame decode for NAME")
            print("uaos timers           pending TimeRequest queue")
            print("uaos stack NAME       stack watermark for NAME")
            return
        sub = args[0]
        if sub == "tasks":
            for i, t in _tasks():
                _dump_task(i, t)
        elif sub == "task" and len(args) > 1:
            want = args[1].lower()
            for i, t in _tasks():
                if _name(t).lower() == want:
                    _dump_task(i, t, full=True)
                    return
            print("uaos: no task", args[1])
        elif sub == "timers":
            _dump_timers()
        elif sub == "stack" and len(args) > 1:
            want = args[1].lower()
            for i, t in _tasks():
                if _name(t).lower() == want:
                    base = _u64(t["native_stack_base"])
                    size = _u64(t["native_stack_size"])
                    inf = gdb.selected_inferior()
                    data = bytes(inf.read_memory(base, size))
                    peak = size
                    for k in range(8, len(data)):
                        if data[k] != 0xA5:
                            peak = size - k
                            break
                    print(f"{_name(t)}: peak {peak}/{size} bytes "
                          f"({'CANARY DEAD' if int(t['stack_overflowed']) else 'ok'})")
                    return
            print("uaos: no task", args[1])
        else:
            print("uaos: unknown — try 'uaos help'")


UaosCmd()
print("uaos helpers loaded — 'uaos help' for commands")
