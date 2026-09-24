---
type: Documentation
title: Dos Manual.pdf
description: Human-readable UAOS DOS & Scripting Manual (shell commands, scripting, assigns) generated from Dos_Manual.md.
resource: /documentation/Dos Manual.pdf
tags: [documentation, manual, pdf, shell, dos]
timestamp: 2026-09-24T04:26:14Z
sources:
  - id: dos-manual-md
    resource: /documentation/Dos_Manual.md
    title: Dos_Manual.md
---

# Dos Manual.pdf

The user-facing DOS & Scripting Manual for UAOS. Covers shell built-ins, `C:` native commands, Ring-3 userspace commands, scripting/flow control, environment variables, assigns, I/O redirection, and resident commands, plus a quick-reference card.

# Build

Generated with pandoc via the LaTeX/pdfTeX engine:

```
pandoc Dos_Manual.md -o "Dos Manual.pdf" --pdf-engine=pdflatex --toc --toc-depth=2 -V geometry:margin=1in
```

Regenerate whenever `Dos_Manual.md` changes — the PDF is a committed build artifact and has gone stale before (last rebuilt 2026-09-24 after the `runback`/`resload`/`LAB`/`SKIP` docs landed).

# Related

- [manual.pdf](/documentation/manual_pdf.md) — the Technical Reference Manual
