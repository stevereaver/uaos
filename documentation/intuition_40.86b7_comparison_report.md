# intuition.library 40.86b7 Reconstruction → UAOS Comparison Report

Plane: UAOS-338. Sources compared:

- **Reference**: `/home/reaver/workspaces/intuition_40.86b7_020` — manual
  behavioral reconstruction of the Cosmos-patched `intuition.library`
  40.86b7 (020+) binary, SHA-256
  `db9a905fde35c856cac2b536766301dd1d5f0016bc067f72ce4a5f032f088520`.
  Reconstructed routines are cited by code-segment offset (e.g. `$15a6a`).
- **UAOS**: `kernel/exec/intuition_lib.c` + `intuition_lib.h` — the native
  thunk layer that serves guest M68k LVO calls, plus
  `emulation/uaos_m68k_glue.c` for the guest jump table.

UAOS already registers 179 intuition handlers covering essentially the whole
public V40 surface including BOOPSI, screen buffers, preferences, and pub
screen locking. The wins therefore are **semantic fidelity**, not coverage:
several places where UAOS's behavior observably diverges from the real
library in ways guest code can see.

## 1. Guest-visible constant mismatches (highest impact)

Guests compile with real NDK constants and then read/write the flag words
directly (or we interpret them). Our `#define`s don't match the binary's
values, so both directions are wrong.

### 1.1 `Screen.Type` flag word — `intuition_lib.h` 641-648

| Flag | UAOS | Real (40.86 / NDK) |
|---|---|---|
| `CUSTOMSCREEN` | `0x0000` | `0x000F` (low nibble = `SCREENTYPE` mask) |
| `CUSTOMBITMAP` | `0x0008` | `0x0040` |
| `BEHIND` (`SCREENBEHIND`) | `0x0020` | `0x0080` |
| `QUIET` (`SCREENQUIET`) | `0x0040` | `0x0100` |
| `AUTOSCROLL` | `0x0080` | `0x4000` |
| `BEEPING` | — missing | `0x0020` |
| `SCREENHIRES` | — missing | `0x0200` |
| `NS_EXTENDED` | — missing | `0x1000` |

These leak into guest-visible state at `SCR_OFF_FLAGS` (screen+$14):
`OpenScreenTagList` writes `type` into it (line ~4401, ~5803), and
`SA_Behind`/`SA_Quiet`/`SA_AutoScroll`/`SA_ShowTitle` map onto the wrong
bits in `SetScreenAttrsA`/`GetScreenAttrsA` (7049-7331) and the Workbench
screen factory (7413). A guest that tests `screen->Type & CUSTOMBITMAP`
(`0x0040`) on a screen UAOS marked `QUIET` (`0x0040`) reads true; a guest
marking `Type |= AUTOSCROLL` sets a bit UAOS doesn't honor.

Evidence: reconstruction `OpenScreenTagList` `$1d722` stores tag-derived
Type directly; `OpenScreen` `$20d0` clears `$f080` and ORs `$c` (WBENCH|
PUBLIC|SHOWTITLE-consistent bits); `MakeScreen` cluster `$a01a`/`$9e06`
reads Type bit `$4000` (AUTOSCROLL) for scrollbar geometry — all
consistent with the canonical NDK values, not ours.

### 1.2 `Gadget.Flags` word — `intuition_lib.h` 312-326

| Flag | UAOS | Real |
|---|---|---|
| `GFLG_SELECTED` | `0x0040` | `0x0080` (0x40 is `RELHEIGHT`) |
| `GFLG_DISABLED` | `0x0080` | `0x0100` (0x80 is `SELECTED`) |
| `GFLG_LABELITEXT` | `0x1000` | `0x0000` |
| `GFLG_LABELSTRING` | `0x6000` | `0x1000` |
| `GFLG_LABELIMAGE` | `0x4000` | `0x2000` (label mask `0x3000`) |
| `GFLG_STRINGEXTEND` | `0x0400` | `0x0400` ✓ |
| `GFLG_IMAGEDISABLE` | `0x0800` | `0x0800` ✓ |
| `GFLG_TABCYCLE` | — missing | `0x0200` |
| `GFLG_RELSPECIAL` | — missing | `0x4000` |
| `GFLG_SYSGADGET` | `0x2000` | not a Flags bit — sysgadgets live in `GadgetType` (0x2000 is real `LABELIMAGE`) |

Invented border bits collide with real flags: `GFLG_RIGHTBORDER 0x0010` =
real `RELRIGHT`; `GFLG_LEFTBORDER 0x0020` = real `RELWIDTH`;
`GFLG_TOPBORDER 0x0100` = real `DISABLED`; `GFLG_BOTTOMBORDER 0x0200` =
real `TABCYCLE`. Real border-relative positioning lives in the
**Activation** word (`GACT_*BORDER`), not Flags. `GFLG_RELBOTTOM 0x0008`
is the only relative flag that happens to be right.

This is load-bearing, not cosmetic: gadget hit-testing, select/disable
rendering, `OnGadget`/`OffGadget`, `GetGadgetInfo`, and the renderer all
read these bits with the wrong masks. A guest-created disabled gadget
(`Flags = GADGDISABLED = 0x0100`) reads as UAOS `TOPBORDER`; a guest-
selected gadget (`0x0080`) reads as UAOS `DISABLED`.

Evidence: `OnGadget`/`OffGadget` cluster `$1042e`-`$10452` and the
dispatcher `$cf46` toggle `gad+$c` bit `$100` (`GADGDISABLED`) and bit
`$80` (`SELECTED`); string gadget activation at `$91ee` checks
`gadget+$e` `$8000`/`$6000` type bits.

### 1.3 `Gadget.Activation` word — `intuition_lib.h` 328-336

UAOS `GACT_TOGGLESELECT = 0x0004` collides with real `GACT_ENDGADGET`;
`GACT_STRINGRIGHT/CENTER/LONGEST` (0x08/0x10/0x18) overlap real
`FOLLOWMOUSE`/`RIGHTBORDER`/`STRINGLONGEST`. Real map:

```
GADGIMMEDIATE 0x0001   RELVERIFY 0x0002   ENDGADGET 0x0004   FOLLOWMOUSE 0x0008
RIGHTBORDER   0x0010   LEFTBORDER 0x0020  TOPBORDER 0x0040   BOTTOMBORDER 0x0080
TOGGLESELECT  0x0100   STRINGCENTER 0x0200  STRINGRIGHT 0x0400  LONGINT 0x0800
ALTKEYMAP     0x1000   BOOLEXTEND  0x2000   GADGETTYPE mask 0xE000
```

## 2. Function-level divergences

Each entry: UAOS function → reconstructed evidence → impact.

### `intuition_NextObject` (11616) — skips first element, returns block not object
Real `$15664`: reads `*objectListPtr` as a link block; if non-NULL returns
`*(block+$c)` (the stored Object*) and advances `*objectListPtr` to the
block's `ln_Succ`. UAOS reads `*ptrptr` as the object node itself, advances
to its succ **before** returning, and returns the block address — so the
first call silently drops element 0 and every call hands back a pointer
12 bytes below the real object (header, not instance). Fix shape:
`obj = cur + OBJ_HEADER_SIZE; *pp = RD32(cur); return obj` matching the
UAOS header convention, or adopt the real block+$c model.

### `intuition_SysReqHandler` (10913) — consumes the IDCMP message
UAOS dequeues the message with `guest_list_remove_head` and frees it.
Real intuition peeks the class without removing the message — the
canonical Workbench pattern is `SysReqHandler()` in a loop followed by
`GetMsg()`/`ReplyMsg()` on the same queue. Guests that GetMsg after
SysReqHandler will find the queue empty (or hang forever waiting for a
message that was already eaten).

### `intuition_SetEditHook` (11076) — V33-era signature
UAOS takes `(gadget, hook)` and pokes `si->EditHook`. Real `$bfc4`
(V36+): `SetEditHook(hook)` — one argument, stored in `IntuitionBase+$ab8`,
old value returned. A guest passing a Hook* in A0 gets it dereferenced as
a Gadget (`gad+SpecialInfo` read from inside the hook struct) — memory
corruption, not just a wrong return.

### `intuition_DoubleClick` (10586) — ignores Preferences threshold
UAOS computes |c-s| against a hardcoded 500ms. Real `$1c0e`: *borrows*
fields when s>c (not abs-diff), adds
`DoubleClickSeconds*1000000 + DoubleClickMicros` from the live
Preferences in IntuitionBase (`+$88`/`+$8c`), compares only when that
threshold is non-zero. Guests that SetPrefs a different double-click
interval see UAOS ignore it; wrap-around cases compare wrong.

### `intuition_WindowLimits` (10994) — different argument semantics
Real `$15a6a`: each of the four args is processed **only when non-zero**;
min values are *signed*-compared against current width/height, max values
*unsigned*-compared; a failing arg makes the whole call return FALSE and
skips the store; surviving values are written into the guest Window's
Min/Max fields (`+$10`..`+$16`). UAOS always stores all four (including
zeroes, which real treats as "unchanged"), compares nothing, writes only
the host-side `IntuitionSlot`, and always returns TRUE. Guests reading
`window->MinWidth` see stale data; `WindowLimits(w,0,0,0,0)` is a no-op
in real intuition but clobbers UAOS's stored limits.

### `intuition_SetWindowTitles` (6629) — no -1 sentinel, no screen title
Real `$3cbe`: `windowTitle == -1` means "leave unchanged", and the
`screenTitle` arg (A2) repaints the frontmost screen's title bar when the
window's own screen is showing; it also bumps a dirty marker at
`win+$ce`. UAOS ignores A2 entirely and treats any non-NULL A1 as a new
title (so `SetWindowTitles(w, -1, "x")` stores `0xFFFFFFFF` as the title
pointer).

### `intuition_InitRequester` (10631) — clears 40 bytes, real clears $70
Real `$10744` zeroes `$70` (112) bytes — the full Requester incl.
`ReqLayer`, `ReqBorder`, `ReqText`, `ReqGadget`, `BitMap` etc. UAOS's
`REQ_SIZE` is 40, so stale stack garbage survives in fields the real call
would have cleared; guests that InitRequester then partially fill a
Requester inherit junk in fields like `ReqLayer`.

### `intuition_SetDMRequest`/`ClearDMRequest` (8224-8252) — wrong active test
Real `$e624` shares one body: it fails (returns 0) **only** when the
window's current DMRequest has the REQACTIVE bit (`req+$1c` bit `$2000`)
set — a *non-active* DMRequest pointer may be replaced freely, and
`ClearDMRequest` (same body, clears instead) can also fail against an
active one. UAOS's SetDMRequest fails whenever the field is non-NULL
(stricter), and ClearDMRequest always succeeds (looser). Both directions
wrong.

### `intuition_GadgetMouse` (8291) — A1 is a GadgetInfo*, not Window*
UAOS reads `win+12/+14` as `MouseX/MouseY`. Real `$aab8` signature is
`GadgetMouse(gadget, gadgetInfo, point)` — it reads the mouse position
out of the `GadgetInfo` (the thing `GetGadgetInfo`/`IGetGadgetInfo` hands
callers) and the gadget's computed domain. Callers passing a real
GadgetInfo get coordinates read from the wrong fields.

### `intuition_PointInImage` (8353) — NULL and BOOPSI dispatch
Real `$4a04`: NULL image → **TRUE**; non-NULL dispatches through the
`$4b78` image-method worker (`IM_HITTEST`-style method) which handles
BOOPSI images (`image+8 == 0xFFFF`). UAOS does a rectangle test on the
raw struct fields only — NULL probably returns wrong and BOOPSI images
(SysIClass borders, GadTools gadgets) are never asked.

### `DrawImage`/`DrawImageState`/`EraseImage` (7671, 8537, 8379) — no image hook
Real `$4aa0`/`$4b20` route every image through `$4b78`: if `image+8 ==
0xFFFF` the pointer is a BOOPSI object dispatched `IM_DRAW`/`IM_ERASE`
via `*(obj-4)`; otherwise `*(ib+$b5e)` (the classic image hook) runs.
UAOS reads `Width/Height/ImageData` literally — for a BOOPSI "image" the
0xFFFF depth word becomes a fake 8-plane BitMap over garbage pointers.
Any guest drawing sysiclass/bevel images via these vectors misrenders.

### `intuition_BeginRefresh`/`EndRefresh` (6559, 6581) — layer protocol absent
Real `$36f6`/`$3754`: `BeginRefresh` locks the screen's LayerInfo,
sets `win+$18` bit `$01000000`, obtains a per-window semaphore, calls
`layers.library BeginUpdate`, and marks `win+$84` bit `$100` on success.
`EndRefresh` runs `EndUpdate(layer, complete)` and unwinds the lock and
both flag bits. UAOS just toggles a host flag and a damage rect — fine
for UAOS's compositor model, but `WFLG_*` window flag bits and the
layer-lock side effects (guest-visible via `window->Flags`) don't move.

### `intuition_NewObjectA` (11339) — pre-allocates before OM_NEW
Real `$3dae`: bumps the class `+$2c` refcount, dispatches `OM_NEW` with
`a0=class, a2=class` (the class itself is the "object" being told to
create), and returns **whatever the dispatcher returns** — the dispatcher
chain allocates the instance (rootclass OM_NEW does the AllocMem). UAOS
pre-allocates `alloc_boopsi_object`, dispatches OM_NEW with `a2`=that
object, and returns the pre-allocated pointer regardless of the
dispatcher result. Consequence: a **guest-defined class** (MakeClass with
an m68k dispatcher) that follows the real convention —
`obj = DoSuperMethodA(cl, o, msg); if (obj) init(INST_DATA(obj))` — gets
`1` back from UAOS's rootclass and runs off into the weeds. Guest BOOPSI
classes can't work until NewObjectA delegates allocation to the
dispatcher. (UAOS's built-in classes are unaffected since they know the
convention.)

### `intuition_RemoveClass` (11603) — no return value
Real `$a3c` returns the class pointer in D0 both for "removed" and "was
never linked" cases, 0 for NULL. UAOS returns nothing (D0 keeps stale
register contents). Trivial.

### `intuition_LockIBase`/`UnlockIBase` (10962-10977)
Real `$7cd2`: obtains `ib+$ab4` semaphore and returns `d0` (the `which`
argument). `UnlockIBase(lock)` obtains the semaphore iff the lock is
nonzero, then releases — i.e. `UnlockIBase(0)` still releases once. UAOS
implements an incrementing counter instead — harmless for correct callers
but wrong for code that passes the lock token back.

### `intuition_DisplayBeep` (10618) — synchronous 50ms busy-wait
Real `$ddea` walks the screen list (arg NULL/-1 → all screens), flashes
each with the palette-swap ramp, honors `BEEPING` per screen, and returns
immediately — the flash is async via a timer. UAOS ignores the screen
argument and `pause`-spins 50ms inside the LVO, stalling the guest.

### `intuition_HelpControl` (11782) — no return value
Real `$d0f8` returns `4` in D0 (`HC_*` handled-mask). UAOS returns void.
Trivial; group propagation logic itself matches.

### `intuition_SetMouseQueue` / `intuition_LendMenus` (8255, 8280)
Real `$1048e` writes the window's `MouseQueue` word at `win+$9c` and
returns the old value; UAOS only stores host-side — guests reading
`window->MouseQueue` see 0. Real `LendMenus` `$cf0c` stores the borrower
pointer in the *target* window's `win+$d4` field; UAOS keeps a single
host global.

## 3. Jump-table coverage gaps

The reference binary's autoinit vector table has 138 slots (-6 .. -828);
every named UAOS `LVO_INTUITION_*` constant was diffed against it and all
implemented offsets match the real `.fd` numbering exactly. The gaps:

- `AlohaWorkbench` (LVO -402) is not installed — a real V40 vector
  (WorkBench "hello/goodbye" handshake) that Workbench-adjacent tools
  call.
- `intuitionPrivate1` (-576), `intuitionPrivate2` (-672),
  `intuitionPrivate3` (-720), `intuitionPrivate4` (-726) — internal
  vectors some 3.x tools and third-party patches call.
- UAOS's later-era functions (LockScreen -936, GetScreenAttrsA -990,
  StartScreenNotifyTagList -1218, …) occupy slots *beyond* the real
  40.86 table, matching the post-3.1 fd numbering — harmless.
- **No catch-all stubs for intuition/gadtools/graphics JVTs.** DOS and
  exec pre-fill every slot with a logging ILLEGAL stub
  (`uaos_m68k_glue.c` 2587-2604); intuition's `-6..-1236` range only gets
  stubs at the 179 implemented offsets. A JSR to any missing slot
  (e.g. -402) executes zeroed RAM — silent corruption instead of a logged
  "unimpl lvo" miss. Cheap fix: same pre-fill loop for INTUITION_BASE,
  GADTOOLS_BASE, GRAPHICS_BASE.

## 4. Suggested follow-ups (ranked)

1. **Flag-word corrections** — rebase `GFLG_*`, `GACT_*`, and the
   `Screen.Type` constants onto real NDK values; audit every mask use in
   `intuition_lib.c`, gadget rendering, and `OpenScreenTagList` tag
   parsing. (type:bug, area:exec)
2. **NextObject + SysReqHandler + InitRequester fixes** — small,
   isolated, clearly wrong. (type:bug)
3. **SetEditHook signature + IntuitionBase hook slot** (ib+$ab8) —
   needs a guest-visible IntuitionBase field; check what UAOS exposes at
   `INTUITION_BASE+0xab8`. (type:bug)
4. **WindowLimits/SetWindowTitles/DMRequest/DoubleClick semantics** —
   per-arg rules, -1 sentinels, REQACTIVE test, Preferences-sourced
   thresholds. (type:enhancement)
5. **Image-method dispatch** — route DrawImage/DrawImageState/EraseImage/
   PointInImage through a `$4b78`-equivalent hook honoring `image+8 ==
   0xFFFF` BOOPSI dispatch and the default old-style hook. Unblocks
   guest sysiclass rendering. (type:enhancement, area:display)
6. **BOOPSI OM_NEW lifecycle** — let dispatchers allocate; return the
   dispatcher's result. Required for third-party guest classes.
   (type:feature, area:exec)
7. **JVT catch-all stubs + missing vectors** (`AlohaWorkbench`,
   `intuitionPrivate*`). (type:chore)
8. **BeginRefresh/EndRefresh flag semantics** — model the `$36f6`/`$3754`
   flag/lock protocol on the guest Window even if the compositor ignores
   layers. (type:enhancement)

## 5. Implementation status (UAOS-339 .. UAOS-345)

All eight work items were implemented. Status per finding:

| Item | Card | Status |
|---|---|---|
| `Screen.Type`/`GFLG_*`/`GACT_*`/`GTYP_*`/`WFLG_*` constants | UAOS-339 | **Done** — canonical NDK values; all consumers symbolic. `GA_Rel*` tags now set `GFLG_REL*` bits; shared `gadget_box()` resolves RELRIGHT/RELBOTTOM/RELWIDTH/RELHEIGHT for hit-test, render, and OM_SET. `tests/qemu_layout_test.py` updated to the canonical `GFLG_SELECTED=0x0080`. |
| `NextObject`, `SysReqHandler`, `InitRequester`, `SetDMRequest`/`ClearDMRequest`, `RemoveClass`, `HelpControl` | UAOS-340 | **Done** — NextObject advances the cursor and returns `block+12`; SysReqHandler peeks without dequeue/free; `Requester` reshaped to the canonical 0x70-byte layout (Flags 0x1c, ReqGadget 0x10, ReqText 0x18, RWindow 0x48); DM-request fails only on `REQACTIVE`; RemoveClass returns the class; `AmigaWindow` extended so `MoreFlags` (0x84) and `HelpGroup` (0xde) are guest-visible, and HelpControl toggles bit 0x20 across the help group and returns 4. |
| `SetEditHook`, `WindowLimits`, `SetWindowTitles`, `DoubleClick`, `GadgetMouse`, `SetMouseQueue`, `LendMenus`, `LockIBase`, `DisplayBeep` | UAOS-341 | **Done** — SetEditHook is the V40 global-hook swap (hook is stored but not yet invoked by the skeletal string-gadget edit path — noted limitation); WindowLimits implements the real nonzero-arg validation; GadgetMouse reads `GadgetInfo.gi_Window` (A1) and returns gadget-relative mouse; SetMouseQueue/LendMenus write the guest fields (0x9c/0xd4) and return old values. |
| Image-method hook (`$4b78` equivalent) | UAOS-342 | **Done** — `DrawImage`/`EraseImage`/`PointInImage` detect `Image.Depth==0xFFFF` and dispatch IM_DRAW/IM_ERASE/IM_HITTEST through the class with canonical `impDraw`/`impErase`/`impHitTest` messages; IM_* IDs and message layouts canonicalized; `imageclass` objects now lead with a real `struct Image` (Depth marker set) so guests can use them as `Image*`. |
| BOOPSI `OM_NEW` lifecycle | UAOS-343 | **Done** — `NewObjectA` dispatches `OM_NEW` with the class as object arg; rootclass allocates via `UAOS_BOOPSI_AllocForClass`; all 22 built-in dispatchers `UAOS_BOOPSI_SuperDispatch` first, then init the returned instance; guest-hook arg order fixed to `a0=cl, a1=msg, a2=obj`; subclass instance data moved to `obj+InstOffset` (previously overlaid the embedded Gadget/Image header); `PropInfo`/`StringInfo` live at `Gadget.SpecialInfo`; `OM_ADDTAIL`/`OM_REMOVE` do real MinNode link math. |
| JVT catch-alls + missing vectors | UAOS-344 | **Done** — intuition (-6..-1236) and gadtools (-6..-174) JVT ranges pre-filled with `ILLEGAL` dispatch stubs routed to index 250 (`D0=0`); `AlohaWorkbench` (-402) and `intuitionPrivate1-4` now return cleanly instead of executing zeroed RAM. |
| `BeginRefresh`/`EndRefresh` flags | UAOS-345 | **Done** — `WFLG_WINDOWREFRESH` (0x01000000) in `Window.Flags` and bit 0x100 in `Window.MoreFlags` now toggle around the refresh bracket; `WFLG_ZOOMED` corrected to 0x10000000. |

Verification: full ISO build clean; `tests/smoke.sh` 24/24;
`tests/qemu_layout_test.py` exercises `NewObjectA` → super-chain
allocation → IDCMP end-to-end under the refactored BOOPSI path.

## 6. Notes
