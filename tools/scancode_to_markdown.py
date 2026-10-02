#!/usr/bin/env python3
"""Convert a ScanCode Toolkit JSON report to a human-readable Markdown report.

Usage:
    python3 scancode_to_markdown.py <scancode_report.json> [<output.md>]

If <output.md> is omitted, writes alongside the input as <name>.md.
"""
import json
import sys
from datetime import datetime
from pathlib import Path


def fmt_pct(n, total):
    if total <= 0:
        return "0.0%"
    return f"{(n / total) * 100:.1f}%"


def tally_table(rows, value_label="Value", count_label="Count"):
    """Render a list of {value, count} dicts as a markdown table."""
    if not rows:
        return "_None detected._\n"
    out = [f"| {value_label} | {count_label} |", "| --- | ---: |"]
    for r in rows:
        v = r.get("value")
        v = v if v is not None else "_(none)_"
        out.append(f"| {v} | {r.get('count', 0)} |")
    return "\n".join(out) + "\n"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    in_path = Path(sys.argv[1])
    if len(sys.argv) >= 3:
        out_path = Path(sys.argv[2])
    else:
        out_path = in_path.with_suffix(".md")

    with in_path.open() as f:
        d = json.load(f)

    headers = d.get("headers", [])
    hdr = headers[0] if headers else {}
    opts = hdr.get("options", {})
    extra = hdr.get("extra_data", {})
    env = extra.get("system_environment", {})
    summary = d.get("summary", {})
    tallies = d.get("tallies", {})
    files = d.get("files", [])

    # Counts
    total_files = sum(1 for f in files if f.get("type") == "file")
    total_dirs = sum(1 for f in files if f.get("type") == "directory")
    total_size = sum(f.get("size", 0) for f in files if f.get("type") == "file")

    lic_files = [f for f in files if f.get("detected_license_expression")]
    copy_files = [f for f in files if f.get("copyrights")]
    pkg_files = [f for f in files if f.get("package_data")]
    email_files = [f for f in files if f.get("emails")]
    url_files = [f for f in files if f.get("urls")]
    err_files = [f for f in files if f.get("scan_errors")]

    lines = []
    add = lines.append

    # --- Title ---
    add("# ScanCode Report — UAOS\n")
    add(f"_Generated {datetime.now().strftime('%Y-%m-%d %H:%M')} from `{in_path.name}`_\n")

    # --- Tool metadata ---
    add("## Scan Metadata\n")
    add(f"- **Tool:** ScanCode Toolkit v{hdr.get('tool_version', '?')}")
    add(f"- **Output format version:** {hdr.get('output_format_version', '?')}")
    add(f"- **SPDX license list version:** {extra.get('spdx_license_list_version', '?')}")
    add(f"- **Scan start:** {hdr.get('start_timestamp', '?')}")
    add(f"- **Scan end:** {hdr.get('end_timestamp', '?')}")
    add(f"- **Duration:** {hdr.get('duration', '?')}s")
    add(f"- **Platform:** {env.get('platform', '?')} ({env.get('operating_system', '?')})")
    add(f"- **Python:** {env.get('python_version', '?')}")
    add(f"- **Errors:** {len(hdr.get('errors', []))}  |  **Warnings:** {len(hdr.get('warnings', []))}")
    add("")
    add("### Scan options")
    add(f"- Scans: {', '.join(k for k in ('--license','--copyright','--package','--info','--email','--url') if opts.get(k))}")
    add(f"- Post-scan: {', '.join(k for k in ('--classify','--summary','--tallies') if opts.get(k))}")
    ig = opts.get("--ignore", [])
    add(f"- Ignored paths: {', '.join(ig) if ig else '_(none)_'}")
    add(f"- Processes: {opts.get('--processes', 'default')}")
    add("")

    # --- Executive summary ---
    add("## Executive Summary\n")
    add(f"- **Declared license:** `{summary.get('declared_license_expression', '_(none)_')}`")
    add(f"- **Declared copyright holder:** {summary.get('declared_holder', '_(none)_')}")
    add(f"- **Primary language:** {summary.get('primary_language', '_(none)_')}")
    lcs = summary.get("license_clarity_score", {})
    if isinstance(lcs, dict):
        add(f"- **License clarity score:** {lcs.get('score', '?')}/100")
        flags = [k for k in ("declared_license","identification_precision","has_license_text","declared_copyrights","conflicting_license_categories","ambiguous_compound_licensing") if k in lcs]
        if flags:
            flag_str = ", ".join(f"{k}={lcs[k]}" for k in flags)
            add(f"  - ({flag_str})")
    add("")
    add(f"- **Files scanned:** {total_files}")
    add(f"- **Directories scanned:** {total_dirs}")
    add(f"- **Total size:** {total_size / (1024 * 1024):.2f} MB")
    add(f"- **Files with license detections:** {len(lic_files)} ({fmt_pct(len(lic_files), total_files)})")
    add(f"- **Files with copyright detections:** {len(copy_files)} ({fmt_pct(len(copy_files), total_files)})")
    add(f"- **Files with package data:** {len(pkg_files)}")
    add(f"- **Files with emails:** {len(email_files)}")
    add(f"- **Files with URLs:** {len(url_files)}")
    add(f"- **Files with scan errors:** {len(err_files)}")
    add("")

    # --- Tallies ---
    add("## Tallies\n")
    add("### Detected license expressions\n")
    add(tally_table(tallies.get("detected_license_expression", []), "License expression"))
    add("### Copyrights\n")
    add(tally_table(tallies.get("copyrights", []), "Copyright"))
    add("### Holders\n")
    add(tally_table(tallies.get("holders", []), "Holder"))
    add("### Authors\n")
    add(tally_table(tallies.get("authors", []), "Author"))
    add("### Programming languages\n")
    add(tally_table(tallies.get("programming_language", []), "Language"))

    # --- License detections (detailed) ---
    add("## Files with License Detections\n")
    if not lic_files:
        add("_No license detections._\n")
    else:
        add("| File | Detected License | SPDX | Detections |")
        add("| --- | --- | --- | ---: |")
        for f in sorted(lic_files, key=lambda x: x.get("path", "")):
            path = f.get("path", "")
            expr = f.get("detected_license_expression", "") or ""
            spdx = f.get("detected_license_expression_spdx", "") or ""
            nd = len(f.get("license_detections", []))
            add(f"| `{path}` | {expr} | {spdx} | {nd} |")
        add("")
        add("### License detection details\n")
        for f in sorted(lic_files, key=lambda x: x.get("path", "")):
            path = f.get("path", "")
            add(f"#### `{path}`\n")
            for det in f.get("license_detections", []):
                add(f"- **Expression:** `{det.get('license_expression', '')}`  ")
                add(f"  - SPDX: `{det.get('license_expression_spdx', '')}`  ")
                for m in det.get("matches", []):
                    add(f"  - Match: `{m.get('license_expression', '')}` "
                        f"(lines {m.get('start_line')}-{m.get('end_line')}, "
                        f"score {m.get('score', '?')}, "
                        f"coverage {m.get('match_coverage', '?')}%, "
                        f"rule `{m.get('rule_identifier', '')}`, "
                        f"relevance {m.get('rule_relevance', '?')})")
            add("")

    # --- Copyright detections ---
    add("## Files with Copyright Detections\n")
    if not copy_files:
        add("_No copyright detections._\n")
    else:
        add("| File | Copyright | Lines |")
        add("| --- | --- | --- |")
        for f in sorted(copy_files, key=lambda x: x.get("path", "")):
            path = f.get("path", "")
            for c in f.get("copyrights", []):
                txt = (c.get("copyright", "") or "").replace("|", "\\|")
                ln = f"{c.get('start_line', '?')}-{c.get('end_line', '?')}"
                add(f"| `{path}` | {txt} | {ln} |")
        add("")

    # --- Emails ---
    add("## Files with Email Detections\n")
    if not email_files:
        add("_No email detections._\n")
    else:
        add("| File | Email | Lines |")
        add("| --- | --- | --- |")
        for f in sorted(email_files, key=lambda x: x.get("path", "")):
            path = f.get("path", "")
            for e in f.get("emails", []):
                add(f"| `{path}` | `{e.get('email', '')}` | {e.get('start_line', '?')}-{e.get('end_line', '?')} |")
        add("")

    # --- URLs ---
    add("## Files with URL Detections\n")
    if not url_files:
        add("_No URL detections._\n")
    else:
        add("| File | URL | Lines |")
        add("| --- | --- | --- |")
        for f in sorted(url_files, key=lambda x: x.get("path", "")):
            path = f.get("path", "")
            for u in f.get("urls", []):
                url = (u.get("url", "") or "").replace("|", "\\|")
                add(f"| `{path}` | {url} | {u.get('start_line', '?')}-{u.get('end_line', '?')} |")
        add("")

    # --- Scan errors ---
    add("## Scan Errors\n")
    if not err_files:
        add("_No scan errors._\n")
    else:
        add("| File | Error |")
        add("| --- | --- |")
        for f in err_files:
            for e in f.get("scan_errors", []):
                add(f"| `{f.get('path', '')}` | {e.get('message', str(e))} |")
        add("")

    # --- Notice ---
    add("---\n")
    add("### Notice\n")
    add(hdr.get("notice", "").replace("\n", " "))
    add("")

    out_path.write_text("\n".join(lines))
    print(f"Wrote {out_path} ({out_path.stat().st_size} bytes, {len(lines)} lines)")


if __name__ == "__main__":
    main()
