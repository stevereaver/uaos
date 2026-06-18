---
type: Kernel Subsystem
title: UAOS Shell
description: The command-line interface for UAOS.
resource: /kernel/shell/
tags: [shell, cli, commands]
timestamp: 2026-06-18T10:00:00Z
---

# UAOS Shell

The UAOS Shell provides a command-line interface for interacting with the system. It is implemented as a resident command handler and a shell window.

## Shell Window

The Shell window (`shell_win.c`) is a graphical window managed by the WM. It provides:
- Scrollable history.
- Line-editing for input.
- Output redirection to the framebuffer.

## Command Execution

Commands in UAOS can be:
- **Internal**: Built into the shell (e.g., `alias`, `set`).
- **Resident**: Compiled into the kernel but executed as separate logic (e.g., `dir`, `mem`).
- **External**: Loaded from disk (e.g., `C:copy`, `C:format`).

## Scripting

The shell supports basic scripting via `S:Startup-Sequence` and the `execute` command, allowing for automated system initialization.
