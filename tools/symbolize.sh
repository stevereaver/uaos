#!/usr/bin/env bash
# symbolize.sh — resolve kernel addresses from a panic dump / serial log
#
# Usage:
#   tools/symbolize.sh < /tmp/uaos_serial.log
#   tools/symbolize.sh /tmp/uaos_serial.log
#   tools/symbolize.sh 0x1002330 0x1002450 ...
#
# Extracts hex addresses (0x...) from the input, resolves each one that
# lands in the kernel text range, and prints "addr  function+offset" plus
# file:line via addr2line when DWARF info is present (build with -g).
#
# ELF can be overridden with ELF=path/to/kernel.elf.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ELF="${ELF:-${REPO_ROOT}/build/uaos-kernel.elf}"

if [[ ! -f "${ELF}" ]]; then
    echo "symbolize: kernel ELF not found at ${ELF}" >&2
    echo "  run ./scripts/build_iso.sh first, or set ELF=/path/to/elf" >&2
    exit 1
fi

# --- collect addresses ---------------------------------------------------
if [[ $# -gt 0 && -f "${1:-}" ]]; then
    INPUT="$(cat -- "$1")"
elif [[ $# -gt 0 && "${1:-}" =~ ^0x ]]; then
    INPUT="$(printf '%s\n' "$@")"
else
    INPUT="$(cat)"
fi

mapfile -t ADDRS < <(grep -oE '0x[0-9a-fA-F]+' <<<"${INPUT}" | sort -u)

# --- symbol table (nearest-symbol fallback) ------------------------------
mapfile -t SYMS < <(nm -n --defined-only "${ELF}" 2>/dev/null | awk '{print $1, $3}')

nearest_sym() {
    local target="${1#0x}" best_addr="" best_name="" line
    local a n
    for line in "${SYMS[@]}"; do
        a="${line%% *}"; n="${line##* }"
        if (( 16#$a <= 16#$target )); then
            best_addr="$a"; best_name="$n"
        else
            break
        fi
    done
    if [[ -n "${best_name}" ]]; then
        local off=$(( 16#$target - 16#$best_addr ))
        printf '%s+0x%x' "${best_name}" "${off}"
    fi
}

found=0
for addr in "${ADDRS[@]}"; do
    sym="$(nearest_sym "${addr}")"
    [[ -z "${sym}" ]] && continue
    found=1
    line="$(addr2line -f -e "${ELF}" "${addr}" 2>/dev/null | tail -1 || true)"
    if [[ "${line}" == "??"* || -z "${line}" ]]; then
        printf '%s  %s\n' "${addr}" "${sym}"
    else
        printf '%s  %s  (%s)\n' "${addr}" "${sym}" "${line}"
    fi
done

if [[ ${found} -eq 0 ]]; then
    echo "symbolize: no kernel addresses found in input" >&2
    exit 1
fi
