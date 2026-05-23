#!/usr/bin/env bash
# build_html.sh — Generate clean HTML docs from Markdown sources
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CSS="${DIR}/style.css"

build_html() {
    local src="$1"
    local out="$2"
    local title="$3"

    # Extract title from first H1 in the file
    local body
    body="$(pandoc "$src" --from=markdown --to=html5 \
        --no-highlight \
        --shift-heading-level-by=0 \
        2>/dev/null)"

    # Build TOC from headings
    local toc
    toc="$(pandoc "$src" --from=markdown --to=html5 \
        --toc --toc-depth=3 \
        --template=/dev/stdin \
        2>/dev/null <<'TMPL'
$toc$
TMPL
    )" || toc=""

    cat > "$out" <<HTML
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>${title}</title>
  <style>
$(cat "$CSS")
  </style>
</head>
<body>
  <nav id="TOC">
${toc}
  </nav>
  <div id="main">
${body}
  </div>
</body>
</html>
HTML
    echo "Built: $out"
}

build_html "${DIR}/manual.md"        "${DIR}/manual.html"  "UAOS Technical Reference Manual"
build_html "${DIR}/../README.md"     "${DIR}/index.html"   "Ultimate Amiga OS"
rm -f "${DIR}/manual_body.html" "${DIR}/index_body.html"
