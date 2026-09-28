---
type: Tool
title: ui_layout_test.c
description: Host-side unit test for the uitree layout engine — builds UINode trees, runs measure/arrange, asserts rects; gates the ISO build.
resource: /tools/ui_layout_test.c
tags: [tool, test, uitree, layout, host]
status: stable
generated: { by: agent:devin, at: 2026-09-28T23:51:28Z }
---

# ui_layout_test.c

Host-side unit harness for `kernel/display/uitree.c`. Compiles the real
kernel source with plain gcc (uitree is dependency-free by design), builds
UINode trees on a heap-backed `UIArena`, and asserts arranged rects.

## Coverage

103 checks over: single-node fill, VGroup weight distribution and exact
tiling at two window sizes, proportional shrink below natural size,
start-packing when no child has weight, spacer right-alignment, explicit
min/max, `UI_F_CENTER`, `UI_PAGE` shared content rect + tab strip
geometry + `ui_page_tab_rect`, `ui_node_at`/`ui_find` hit-tests, natural
sizes for every leaf kind, `ui_append` ordering and parent links, arena
exhaustion (NULL on overflow), NULL/empty-tree safety, and `pad` insets.

## Build and run

```bash
gcc -O2 -I kernel/display -o build/ui_layout_test \
    tools/ui_layout_test.c kernel/display/uitree.c
./build/ui_layout_test     # prints "N checks, M failed"; exit 1 on fail
```

`scripts/build_iso.sh` step 1a builds and runs it before compiling the
kernel, so a layout regression aborts the ISO build (`set -e`).

## Related

- [uitree](/kernel/display/uitree.md) — the engine under test.
