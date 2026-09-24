---
type: Documentation
title: manual.pdf
description: Human-readable UAOS Technical Reference Manual (architecture, kernel subsystems, build/run instructions) compiled from manual.tex.
resource: /documentation/manual.pdf
tags: [documentation, manual, pdf, architecture]
timestamp: 2026-09-24T04:26:14Z
sources:
  - id: manual-tex
    resource: /documentation/manual.tex
    title: manual.tex
  - id: manual-md
    resource: /documentation/manual.md
    title: manual.md
---

# manual.pdf

The Technical Reference Manual for UAOS: system architecture, kernel subsystems, ROM modules, graphics/WM, input drivers, filesystem layer, shell, TCP/IP networking, build system, QEMU usage, troubleshooting, and the memory map.

# Build

Compiled directly from the committed `manual.tex` with pdfLaTeX (run twice so the TOC/LoF/LoT resolve):

```
pdflatex -interaction=nonstopmode -halt-on-error manual.tex
pdflatex -interaction=nonstopmode -halt-on-error manual.tex
```

`manual.md` is the Markdown sibling rendered to `manual.html` by `build_html.sh`; keep `manual.tex`, `manual.md`, and the PDF in sync. Regeneration leaves refreshed `manual.aux/.log/.toc/.lof/.lot/.out` artifacts alongside the PDF.

# Related

- [Dos Manual.pdf](/documentation/dos_manual_pdf.md) — the DOS & Scripting Manual
