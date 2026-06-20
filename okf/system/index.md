---
type: System Layout
title: UAOS System Layout
description: Overview of the Amiga-style system directory structure in UAOS.
tags: [layout, filesystem, workbench]
timestamp: 2026-06-18T10:00:00Z
---

# UAOS System Layout

UAOS follows the classic AmigaOS directory structure to maintain compatibility and familiarity for Amiga users.

## Core Directories

- **`C:` (Commands)**: Shell commands and utilities (e.g., `dir`, `copy`, `format`).
- **`DEVS:` (Devices)**: Device drivers and configuration files.
- **`L:` (Loadable Handlers)**: Filesystem and device handlers (e.g., `FastFileSystem`, `aux-handler`).
- **`LIBS:` (Libraries)**: Shared libraries.
- **`S:` (Scripts)**: System scripts, including `Startup-Sequence`.
- **`SYS:` (System Utilities)**: Core system tools and the Workbench itself.

## Special Assigns

UAOS uses "Assigns" to create logical device names:
- **`Workbench:`**: The main boot volume.
- **`T:`**: Temporary directory (often in RAM:).
- **`ENV:`**: Environment variables.
- **`CLIPS:`**: Clipboard data.

## Filesystem Skeleton

The `system/` directory in the repository contains the template for the UAOS system root, which is packaged into the boot ISO.
