---
name: plane-tracking
description: REQUIRED for every task — create a Plane card for the work, keep it updated while working, and close it out when done. Tracks all work as cards in the UAOS project on plane.reavernet via the plane MCP server.
---

All work in this workspace is tracked as cards (work items) in Plane. Use the `plane` MCP server for everything below.

## Project

Default project: **UAOS**
- `project_id`: `5cafe78b-fcf6-4a5b-a461-cba2309e563f`
- Identifier prefix: `UAOS` (cards look like `UAOS-7`)

If the user names a different project, resolve its `project_id` first: `project list`, then match on `name` or `identifier`.

## Workflow

### 1. At the start of any task

- If the user references an existing card (e.g. `UAOS-12`), fetch it with
  `workitem retrieve_by_identifier` and work against that card.
- If the user describes new work, search first to avoid duplicates
  (`workitem search` or `workitem list` with a PQL filter), then create a card:
  `workitem create` with `project_id`, a clear `name`, and `description_html`
  summarizing the request and acceptance criteria.
- Set the card's state to the in-progress state. Resolve state UUIDs with
  `state list` (project_id) and pick the state whose `group` is `started`
  (typically "In Progress").
- Assign the card to the requesting user when asked (`member list_workspace` to
  resolve the member id, then `workitem manage_assignee`).

## Labels

Two-axis taxonomy (created UAOS-30). Apply an `area:*` label plus a `type:*`
label to every card you create; add a second `area:*` when work genuinely spans
subsystems. Resolve ids with `label list`, then `workitem manage_label`.

- `area:exec` — Exec + AmigaOS-compatible libraries (scheduling, memory, IPC, math libs)
- `area:dos` — packet handlers, filesystems (FAT32/ISO9660/RAMFS), VFS, volumes, shell commands
- `area:drivers` — hardware drivers: VirtIO, block layer, IDE/ATAPI, partitions
- `area:display` — rendering, bitplanes, palette, screens, pointer, blanker
- `area:desktop` — Workbench desktop UX: icons, menus, windows, requesters, gadgets
- `area:audio` — audio.device, speaker, sound
- `area:meta` — process, tooling, planning, repo maintenance (not OS code)
- `type:feature` — new functionality
- `type:enhancement` — improvement/polish to existing behavior
- `type:bug` — defect fix or corrective work
- `type:tracking` — tracking/umbrella card grouping other work
- `type:chore` — maintenance, hygiene, or process work

### 2. While working

- Keep the card as the source of truth. When scope changes, update
  `name`/`description_html` via `workitem update`.
- Post a `workitem_comment create` (`comment_html`) at meaningful milestones:
  findings, design decisions, blockers, and anything the user should see in
  Plane rather than only in chat.
- Add/remove labels with `workitem manage_label` when the user asks
  (`label list` to resolve label ids).

### 3. When the work is done

- Post a final `workitem_comment create` summarizing what changed, how it was
  verified, and relevant file/commit references.
- Move the card to the completed state (`workitem update` with the state UUID
  whose `group` is `completed`). If the work was abandoned or rejected, use the
  `cancelled` group instead and say why in the closing comment.
- Never delete cards unless the user explicitly asks.

## Notes

- Always report the card identifier (e.g. `UAOS-7`) back to the user so they
  can find it in Plane.
- For complex filtering use PQL; call `get_pql_reference` before composing
  non-trivial queries.
- Sub-tasks of a card are created with `workitem create` + `parent`.
- If the `plane` MCP server is unavailable, tell the user once, continue the
  work, and offer to sync the card afterwards.
- Skip creating work items for pure questions/explanations where nothing is
  changed; use judgment for trivial asks.
- If the MCP call fails (plane.reavernet unreachable), mention it and continue
  the task — do not block on it.
