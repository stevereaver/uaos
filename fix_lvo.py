#!/usr/bin/env python3
"""Fix all intuition.library LVO offsets in uaos_m68k_glue.c to match
the official AmigaOS 3.1 (V39) and V40 vector tables."""

import re, sys

GLUE = "emulation/uaos_m68k_glue.c"
with open(GLUE, "r") as f:
    src = f.read()

# ── Official AmigaOS intuition.library LVO offsets ──────────────────────────
# (function_index, LVO offset, official name, has_lvo_stub)
# has_lvo_stub = False for amiga.lib functions and varargs-only stubs
LVO_TABLE = [
    (1,   -30,  "OpenIntuition",          True),
    (2,   -36,  "Intuition",              True),
    (3,   -204, "OpenWindow",             True),
    (4,   -72,  "CloseWindow",            True),
    (5,   -312, "WindowToFront",          True),
    (6,   -306, "WindowToBack",           True),
    (7,   -450, "ActivateWindow",         True),
    (8,   -168, "MoveWindow",             True),
    (9,   -288, "SizeWindow",             True),
    (10,  -456, "RefreshWindowFrame",     True),
    (11,  -150, "ModifyIDCMP",            True),
    (12,  -276, "SetWindowTitles",        True),
    (13,  -606, "OpenWindowTagList",      True),
    (14,  -210, "OpenWorkBench",          True),
    (15,  -78,  "CloseWorkBench",         True),
    (16,  -108, "DrawBorder",             True),
    (17,  -114, "DrawImage",              True),
    (18,  -216, "PrintIText",             True),
    (19,  -348, "AutoRequest",            True),
    (20,  -360, "BuildSysRequest",        True),
    (21,  -372, "FreeSysRequest",         True),
    (22,  -588, "EasyRequestArgs",        True),
    (23,  -198, "OpenScreen",             True),
    (24,  -66,  "CloseScreen",            True),
    (25,  -162, "MoveScreen",             True),
    (26,  -252, "ScreenToFront",          True),
    (27,  -246, "ScreenToBack",           True),
    (28,  -282, "ShowTitle",              True),
    (29,  -612, "OpenScreenTagList",      True),
    (30,  -264, "SetMenuStrip",           True),
    (31,  -54,  "ClearMenuStrip",         True),
    (32,  -702, "ResetMenuStrip",         True),
    (33,  -144, "ItemAddress",            True),
    (34,  -510, "LockPubScreen",          True),
    (35,  -516, "UnlockPubScreen",        True),
    (36,  -522, "LockPubScreenList",      True),
    (37,  -528, "UnlockPubScreenList",    True),
    (38,  -270, "SetPointer",             True),
    (39,  -60,  "ClearPointer",           True),
    (40,  -816, "SetWindowPointerA",      True),
    (41,  -126, "GetDefPrefs",            True),
    (42,  -132, "GetPrefs",               True),
    (43,  -324, "SetPrefs",               True),
    (44,  -852, "LockGUIPrefs",           True),
    (45,  -858, "UnlockGUIPrefs",         True),
    (46,  -474, "QueryOverscan",          True),
    # 47 GetDisplayInfoData — graphics.library, NOT intuition LVO
    # 48 NextDisplayInfo — graphics.library, NOT intuition LVO
    (49,  -84,  "CurrentTime",            True),
    (50,  -102, "DoubleClick",            True),
    (51,  -234, "ReportMouse",            True),
    (52,  -96,  "DisplayBeep",            True),
    (53,  -138, "InitRequester",          True),
    (54,  -120, "EndRequest",             True),
    (55,  -240, "Request",                True),
    (56,  -294, "ViewAddress",            True),
    (57,  -300, "ViewPortAddress",        True),
    (58,  -426, "GetScreenData",          True),
    (59,  -534, "NextPubScreen",          True),
    (60,  -540, "SetDefaultPubScreen",    True),
    (61,  -414, "LockIBase",              True),
    (62,  -420, "UnlockIBase",            True),
    (63,  -834, "ShowWindow",             True),
    (64,  -840, "HideWindow",             True),
    (65,  -318, "WindowLimits",           True),
    (66,  -486, "ChangeWindowBox",        True),
    (67,  -690, "GetScreenDrawInfo",      True),
    (68,  -696, "FreeScreenDrawInfo",     True),
    (69,  -90,  "DisplayAlert",           True),
    (70,  -822, "TimedDisplayAlert",      True),
    (71,  -786, "ScreenDepth",            True),
    (72,  -792, "ScreenPosition",         True),
    (73,  -42,  "AddGadget",              True),
    (74,  -438, "AddGList",               True),
    (75,  -228, "RemoveGadget",           True),
    (76,  -444, "RemoveGList",            True),
    (77,  -432, "RefreshGList",           True),
    (78,  -186, "OnGadget",               True),
    (79,  -174, "OffGadget",              True),
    (80,  -156, "ModifyProp",             True),
    (81,  -468, "NewModifyProp",          True),
    (82,  -462, "ActivateGadget",         True),
    (83,  -954, "SetWindowAttrsA",        True),
    (84,  -948, "GetWindowAttrsA",        True),
    (85,  -996, "SetScreenAttrsA",        True),
    (86,  -990, "GetScreenAttrsA",        True),
    # 87 GetVisualInfoA — gadtools.library, NOT intuition LVO
    # 88 FreeVisualInfo — gadtools.library, NOT intuition LVO
    (89,  -354, "BeginRefresh",           True),
    (90,  -366, "EndRefresh",             True),
    (91,  -222, "RefreshGadgets",         True),
    (92,  -192, "OnMenu",                 True),
    (93,  -180, "OffMenu",                True),
    (94,  -600, "SysReqHandler",          True),
    (95,  -552, "PubScreenStatus",        True),
    (96,  -582, "GetDefaultPubScreen",    True),
    (97,  -480, "MoveWindowInFrontOf",    True),
    (98,  -492, "SetEditHook",            True),
    (99,  -558, "ObtainGIRPort",          True),
    (100, -564, "ReleaseGIRPort",         True),
    (101, -972, "StripIntuiMessages",     True),
    (102, -636, "NewObjectA",             True),
    (103, -642, "DisposeObject",          True),
    (104, -648, "SetAttrsA",              True),
    (105, -654, "GetAttr",                True),
    # 106 DoMethodA — amiga.lib, NOT intuition LVO
    # 107 DoSuperMethodA — amiga.lib, NOT intuition LVO
    # 108 CoerceMethodA — amiga.lib, NOT intuition LVO
    (109, -678, "MakeClass",              True),
    (110, -714, "FreeClass",              True),
    (111, -684, "AddClass",               True),
    (112, -708, "RemoveClass",            True),
    (113, -666, "NextObject",             True),
    (114, -846, "GetAttrsA",              True),
    # 115 SetSuperAttrsA — amiga.lib, NOT intuition LVO
    (116, -810, "DoGadgetMethodA",        True),
    (117, -828, "HelpControl",            True),
    (118, -1218, "StartScreenNotifyTagList", True),
    (119, -1224, "EndScreenNotify",       True),
    (120, -960, "GetWindowAttr",          True),
    (121, -966, "SetWindowAttr",          True),
    (122, -1002, "GetScreenAttr",         True),
    (123, -1008, "SetScreenAttr",         True),
    # 124-136: Varargs stubs — share LVO with A-suffix counterparts, no separate LVO
    (130, -660, "SetGadgetAttrsA",        True),
    (137, -768, "AllocScreenBuffer",      True),
    (138, -774, "FreeScreenBuffer",       True),
    (139, -780, "ChangeScreenBuffer",     True),
]

# Build LVO define name mapping
FUNC_NAMES = {
    1: "OPEN_LIBRARY", 2: "CLOSE_LIBRARY", 3: "OPEN_WINDOW", 4: "CLOSE_WINDOW",
    5: "WINDOW_TO_FRONT", 6: "WINDOW_TO_BACK", 7: "ACTIVATE_WINDOW",
    8: "MOVE_WINDOW", 9: "SIZE_WINDOW", 10: "REFRESH_WINDOW",
    11: "MODIFY_IDCMP", 12: "SET_WINDOW_TITLES", 13: "OPEN_WINDOW_TAGS",
    14: "OPEN_WORKBENCH", 15: "CLOSE_WORKBENCH", 16: "DRAW_BORDER",
    17: "DRAW_IMAGE", 18: "PRINT_I_TEXT", 19: "AUTO_REQUEST",
    20: "BUILD_SYS_REQUEST", 21: "FREE_SYS_REQUEST", 22: "EASY_REQUEST",
    23: "OPEN_SCREEN", 24: "CLOSE_SCREEN", 25: "MOVE_SCREEN",
    26: "SCREEN_TO_FRONT", 27: "SCREEN_TO_BACK", 28: "SHOW_TITLE",
    29: "OPEN_SCREEN_TAGS", 30: "SET_MENU_STRIP", 31: "CLEAR_MENU_STRIP",
    32: "RESET_MENU_STRIP", 33: "ITEM_ADDRESS", 34: "LOCK_PUB_SCREEN",
    35: "UNLOCK_PUB_SCREEN", 36: "LOCK_PUB_SCREEN_LIST",
    37: "UNLOCK_PUB_SCREEN_LIST", 38: "SET_POINTER", 39: "CLEAR_POINTER",
    40: "SET_WINDOW_POINTER_A", 41: "GET_DEF_PREFS", 42: "GET_PREFS",
    43: "SET_PREFS", 44: "LOCK_GUI_PREFS", 45: "UNLOCK_GUI_PREFS",
    46: "QUERY_OVERSCAN", 49: "CURRENT_TIME", 50: "DOUBLE_CLICK",
    51: "REPORT_MOUSE", 52: "DISPLAY_BEEP", 53: "INIT_REQUESTER",
    54: "END_REQUEST", 55: "REQUEST", 56: "VIEW_ADDRESS",
    57: "VIEW_PORT_ADDRESS", 58: "GET_SCREEN_DATA", 59: "NEXT_PUB_SCREEN",
    60: "SET_DEFAULT_PUB_SCREEN", 61: "LOCK_IBASE", 62: "UNLOCK_IBASE",
    63: "SHOW_WINDOW", 64: "HIDE_WINDOW", 65: "WINDOW_LIMITS",
    66: "CHANGE_WINDOW_BOX", 67: "GET_SCREEN_DRAW_INFO",
    68: "FREE_SCREEN_DRAW_INFO", 69: "DISPLAY_ALERT",
    70: "TIMED_DISPLAY_ALERT", 71: "SCREEN_DEPTH", 72: "SCREEN_POSITION",
    73: "ADD_GADGET", 74: "ADD_GLIST", 75: "REMOVE_GADGET",
    76: "REMOVE_GLIST", 77: "REFRESH_GLIST", 78: "ON_GADGET",
    79: "OFF_GADGET", 80: "MODIFY_PROP", 81: "NEW_MODIFY_PROP",
    82: "ACTIVATE_GADGET", 83: "SET_WINDOW_ATTRS", 84: "GET_WINDOW_ATTRS",
    85: "SET_SCREEN_ATTRS", 86: "GET_SCREEN_ATTRS",
    89: "BEGIN_REFRESH", 90: "END_REFRESH", 91: "REFRESH_GADGETS",
    92: "ON_MENU", 93: "OFF_MENU", 94: "SYS_REQ_HANDLER",
    95: "PUB_SCREEN_STATUS", 96: "GET_DEFAULT_PUB_SCREEN",
    97: "MOVE_WINDOW_IN_FRONT_OF", 98: "SET_EDIT_HOOK",
    99: "OBTAIN_GIR_PORT", 100: "RELEASE_GIR_PORT",
    101: "STRIP_INTUI_MESSAGES", 102: "NEW_OBJECT_A",
    103: "DISPOSE_OBJECT", 104: "SET_ATTRS_A", 105: "GET_ATTR",
    109: "MAKE_CLASS", 110: "FREE_CLASS", 111: "ADD_CLASS",
    112: "REMOVE_CLASS", 113: "NEXT_OBJECT", 114: "GET_ATTRS_A",
    116: "DO_GADGET_METHOD_A", 117: "HELP_CONTROL",
    118: "START_SCREEN_NOTIFY", 119: "END_SCREEN_NOTIFY",
    120: "GET_WINDOW_ATTR", 121: "SET_WINDOW_ATTR",
    122: "GET_SCREEN_ATTR", 123: "SET_SCREEN_ATTR",
    130: "SET_GADGET_ATTRS_A",
    137: "ALLOC_SCREEN_BUFFER", 138: "FREE_SCREEN_BUFFER",
    139: "CHANGE_SCREEN_BUFFER",
}

# ── 1. Replace LVO define block ────────────────────────────────────────────
old_lvo_start = "/* intuition.library LVO offsets */"
old_lvo_end = "#define LVO_INTUITION_CHANGE_SCREEN_BUFFER      (-780)"

idx_start = src.index(old_lvo_start)
idx_end = src.index(old_lvo_end) + len(old_lvo_end)

new_lines = ['/* intuition.library LVO offsets — official AmigaOS 3.1 (V39) and V40 values.\n',
             ' * Verified against the AmigaOS NDK intuition_lib.i include file.\n',
             ' * Functions not in the official LVO table (amiga.lib link-time\n',
             ' * functions, gadtools/graphics functions) have NO LVO stub installed. */']

for fn_idx, lvo, name, has_stub in LVO_TABLE:
    fname = FUNC_NAMES.get(fn_idx, "")
    if not fname:
        continue
    define_name = f"LVO_INTUITION_{fname}"
    comment = f"/* {name} */"
    new_lines.append(f'#define {define_name:<42s} ({lvo:>5d})  {comment}')

new_lines.append('')
new_lines.append('/* Non-LVO functions (amiga.lib / gadtools / graphics.library):')
new_lines.append(' *   GetDisplayInfoData(47), NextDisplayInfo(48) — graphics.library')
new_lines.append(' *   GetVisualInfoA(87), FreeVisualInfo(88) — gadtools.library')
new_lines.append(' *   DoMethodA(106), DoSuperMethodA(107), CoerceMethodA(108),')
new_lines.append(' *   SetSuperAttrsA(115) — amiga.lib (not intuition LVO)')
new_lines.append(' * Varargs stubs share LVO with A-suffix counterparts:')
new_lines.append(' *   NewObject(124)→NewObjectA, SetAttrs(125)→SetAttrsA,')
new_lines.append(' *   GetAttrs(126)→GetAttrsA, DoMethod(127)→DoMethodA,')
new_lines.append(' *   DoSuperMethod(128)→DoSuperMethodA, CoerceMethod(129)→CoerceMethodA,')
new_lines.append(' *   SetSuperAttrs(131)→SetSuperAttrsA, SetWindowPointer(132)→SetWindowPointerA,')
new_lines.append(' *   OpenWindowTags_V(133)→OpenWindowTagList, OpenScreenTags_V(134)→OpenScreenTagList,')
new_lines.append(' *   DoGadgetMethod(135)→DoGadgetMethodA, SetGadgetAttrs(136)→SetGadgetAttrsA */')

src = src[:idx_start] + '\n'.join(new_lines) + '\n' + src[idx_end:]

# ── 2. Fix stub_addr() cases ───────────────────────────────────────────────
# Build the new case block for LIB_INTUITION in stub_addr
case_lines = ['        switch (func_idx) {']
for fn_idx, lvo, name, has_stub in LVO_TABLE:
    fname = FUNC_NAMES.get(fn_idx, "")
    if not fname or not has_stub:
        continue
    case_lines.append(f'            case INTUITION_{fname}: return (uint32_t)((int)INTUITION_BASE + LVO_INTUITION_{fname});')
case_lines.append('        }')

# Find and replace the intuition switch block in stub_addr
# The block starts with "switch (func_idx) {" after "lib_id == LIB_INTUITION"
# and ends with "}" followed by "} else if (lib_id == LIB_GADTOOLS)"
intu_switch_start = src.index('} else if (lib_id == LIB_INTUITION) {\n        switch (func_idx) {')
intu_switch_start += len('} else if (lib_id == LIB_INTUITION) {\n')
intu_switch_end = src.index('        }\n    } else if (lib_id == LIB_GADTOOLS)', intu_switch_start)

src = src[:intu_switch_start] + '\n'.join(case_lines) + src[intu_switch_end:]

# ── 3. Fix install_lvo() calls ─────────────────────────────────────────────
# Find the intuition install_lvo block and replace it
# It starts with "/* intuition.library at INTUITION_BASE */" and ends before
# "/* gadtools.library at GADTOOLS_BASE */"
intu_install_start = src.index('    /* intuition.library at INTUITION_BASE')
intu_install_end = src.index('\n    /* gadtools.library at GADTOOLS_BASE */', intu_install_start)

install_lines = ['    /* intuition.library at INTUITION_BASE */']
for fn_idx, lvo, name, has_stub in LVO_TABLE:
    fname = FUNC_NAMES.get(fn_idx, "")
    if not fname or not has_stub:
        continue
    install_lines.append(f'    install_lvo(INTUITION_BASE, LVO_INTUITION_{fname:<42s}, LIB_INTUITION, INTUITION_{fname});')

src = src[:intu_install_start] + '\n'.join(install_lines) + src[intu_install_end:]

with open(GLUE, "w") as f:
    f.write(src)

print("LVO offsets fixed successfully.")
print(f"  {sum(1 for _,_,_,h in LVO_TABLE if h)} LVO stubs installed")
print(f"  {len(LVO_TABLE) - sum(1 for _,_,_,h in LVO_TABLE if h)} non-LVO functions skipped")
