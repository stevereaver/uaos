#!/usr/bin/env bash
# build_iso.sh — compatibility wrapper around the top-level Makefile.
#
# The build is now driven by the repo-root Makefile (parallel via make -j,
# incremental via file-level dependencies).  This script is kept so existing
# documentation, CI, and muscle memory keep working:
#
#   ./scripts/build_iso.sh            # make -j$(nproc) iso
#   ./scripts/build_iso.sh --clean    # make clean, then build
#   ./scripts/build_iso.sh kernel     # forward any other args to make
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ "${1:-}" == "--clean" ]]; then
    make -C "${REPO_ROOT}" clean
    shift
fi

exec make -C "${REPO_ROOT}" -j"$(nproc)" "$@"
