---
name: okf
description: Scan the OKF (Open Knowledge Format) bundle before taking any action, then update it after completing. Use this skill as a read-before / write-after wrapper around any workspace task.
argument-hint: "<task description>"
allowed-tools:
  - read
  - edit
  - write
  - grep
  - glob
  - exec
triggers:
  - user
  - model
---

# OKF Scan-Then-Update Workflow

This workspace maintains an **OKF (Open Knowledge Format) bundle** at `okf/`
that documents everything in it: binaries, disassembly, tools, and any other
concepts. Before you perform any action in this workspace you must **scan**
the OKF bundle to load current context, and after you finish you must
**update** the OKF bundle to reflect what changed.

Treat this as a mandatory read-before / write-after wrapper around the user's
actual request. Never skip either phase.

---

## Phase 1 — Scan OKF (before acting)

Before doing the user's requested work, build a mental model of the current
state of the workspace from the OKF bundle:

1. Read `okf/index.md` — the root index. Its frontmatter carries
   `okf_version`; its body lists every concept grouped by category with
   one-line descriptions and links into the subdirectories.
2. Read `okf/log.md` — the chronological change log. The most recent entries
   tell you what was last done and when.
3. For each category the task is likely to touch, read that category's
   `okf/<category>/index.md` and the individual concept files referenced
   from it. Categories seen so far: `drivers`, `disassembly`, `tools`. New
   categories may exist or need creating — discover them by listing `okf/`.
4. Note the `resource:` field in each concept's frontmatter — it is the
   workspace-relative path to the real artifact the concept documents
   (e.g. `/bin/ksthunk.sys`, `/asm/ksthunk.sys.asm`, `/disasm.py`).

Summarize what you found to the user in 2-4 lines before proceeding, so it
is clear you grounded yourself in the existing knowledge. Then do the
user's actual task.

If the `okf/` directory does not exist, say so and ask the user whether they
want it initialized before continuing.

---

## Phase 2 — Do the task

Perform the user's requested work normally. Use whichever tools the task
requires. Keep track of:
- Which workspace files you created, modified, or deleted.
- Which concepts (documented artifacts) are affected.
- Whether any brand-new category of artifact appeared that OKF does not
  represent yet.

---

## Phase 3 — Update OKF (after acting)

After the task is complete, bring the OKF bundle up to date so it stays an
accurate map of the workspace. Follow the existing format exactly.

### Concept file format

Each documented artifact is one Markdown file under `okf/<category>/`.
Filename is the artifact name with non-alphanumeric chars turned to `_`
and `.md` appended (e.g. `ksthunk.sys` -> `ksthunk_sys.md`,
`ksthunk.sys.asm` -> `ksthunk_sys_asm.md`, `disasm.py` -> `disasm_py.md`).

Frontmatter fields:

```yaml
---
type: <Concept type>          # e.g. PE Binary, Disassembly, Tool
title: <artifact name>        # original filename / name
description: <one-line summary>
resource: /<workspace-relative path>   # path to the real artifact
tags: [<lowercase tags>]
status: stable                # stable | draft | deprecated
generated: { by: <human:devin | process:<name> | agent:devin>, at: <ISO 8601 UTC> }
sources:                      # optional, for derived artifacts
  - id: <short id>
    resource: /<path>
    title: <source name>
---
```

Body sections seen in this bundle: `# Overview`, `# PE Header` / `# Behavior`
/ `# Disassembled sections` (type-specific), `# Format`, `# Dependencies`,
`# Examples`, `# Related`. Use `# Related` to cross-link sibling concepts
with relative paths (e.g. `[disasm.py](/tools/disasm_py.md)` — note the
leading `/` makes it resolve from the bundle root).

### Update steps

1. **Add or update concept files** for every artifact you created, changed,
   or removed.
   - New artifact -> create `okf/<category>/<name>.md` using the format
     above. Pick or create a sensible category directory.
   - Changed artifact -> edit the existing concept file to reflect new
     facts (size, section table, behavior, status...). Bump its
     `generated.at` timestamp.
   - Deleted artifact -> remove its concept file and drop it from the
     category index and root index.
2. **Update each category `index.md`** (`okf/<category>/index.md`) so its
   bullet list matches the concept files present. Keep the one-line
   descriptions current. Create the category directory and its `index.md`
   if it is brand new.
3. **Update the root `okf/index.md`** so every category is listed under a
   `# <Category>` heading with links to its concept files, matching the
   existing grouping style. Keep the frontmatter `okf_version` intact.
4. **Append to `okf/log.md`** under a `## <YYYY-MM-DD>` heading for today's
   date (create the heading if today has none). Add one bullet per
   significant change, prefixed with the action verb in bold, e.g.:
   `* **Created**: ...`, `* **Updated**: ...`, `* **Removed**: ...`.
   Match the terse style of existing log entries.

### Timestamps

Use the current UTC time in ISO 8601 (`YYYY-MM-DDTHH:MM:SSZ`). Run
`date -u +%Y-%m-%dT%H:%M:%SZ` (or the PowerShell equivalent
`Get-Date -AsUTC -Format 'yyyy-MM-ddTHH:mm:ssZ'`) to get it — do not guess.

---

## Summary to user

When both phases are done, report:
1. What the task accomplished (the real work).
2. Which OKF files you scanned and which you created/updated, with a
   one-line note on each.

Keep it concise. The OKF bundle should always be the source of truth for
what is in this workspace after you leave.
