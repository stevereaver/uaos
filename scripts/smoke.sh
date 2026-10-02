#!/usr/bin/env bash
# smoke.sh — headless QEMU boot + telnet command battery (UAOS-215)
#
# Codifies the manual "QEMU-verified" ritual: builds nothing (expects the
# ISO to exist — run scripts/build_iso.sh first), boots run_with_disk.sh
# headless, drives the telnet port, asserts each command produces output,
# then archives serial log + pcap to build/smoke-<ts>/.
#
#   scripts/smoke.sh [--timeout 90] [--keep]
#
# Requires: qemu run script working, telnet forward on host :2323,
# nc (netcat) available.

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TIMEOUT=90
KEEP=0
TELNET_PORT=2323
SERIAL_LOG=/tmp/uaos_serial.log
PCAP=/tmp/uaos_net.pcap

while [ $# -gt 0 ]; do
    case "$1" in
        --timeout) TIMEOUT="$2"; shift 2;;
        --keep)    KEEP=1; shift;;
        *) echo "usage: $0 [--timeout N] [--keep]"; exit 2;;
    esac
done

TS="$(date +%Y%m%d-%H%M%S)"
OUT="${REPO_ROOT}/build/smoke-${TS}"
mkdir -p "${OUT}"
echo "smoke: results -> ${OUT}"

# --- start qemu ----------------------------------------------------------
: >"${SERIAL_LOG}"
"${REPO_ROOT}/scripts/run_with_disk.sh" >"${OUT}/qemu.out" 2>&1 &
QPID=$!
trap 'kill ${QPID} 2>/dev/null' EXIT

# wait for telnet port
wait_port() {
    for _ in $(seq 1 ${TIMEOUT}); do
        if echo -e '\x1d' | nc -w 1 127.0.0.1 ${TELNET_PORT} >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

echo "smoke: waiting for telnetd (:${TELNET_PORT}, up to ${TIMEOUT}s) ..."
if ! wait_port; then
    echo "smoke: FAIL — telnet port never came up"
    echo "smoke: serial tail:"; tail -30 "${SERIAL_LOG}" || true
    exit 1
fi

# --- drive the battery ---------------------------------------------------
# Each entry: "command|expected_substring" ('.' = any output ok)
BATTERY="
version|.
dir|.
mem|.
ps|.
taskdump|pri=
taskstat|window
watchdog|watchdog
ports|.
timers|.
handles|.
netstat|TCP
diskdiag|block devices
pciscan|bdf
irqroute|pin
irqaudit|crit
sercon|sercon
tickcheck|tsc_hz
etrace|etrace
prof|prof
failalloc|failalloc
pktmon|pktmon
stack|peak
irqstat|vec
dmesg 5|.
"

PASS=0; FAIL=0
send_cmd() {
    # open a fresh telnet session per command — simpler than readline sync
    local cmd="$1"
    { sleep 1; printf '%s\r' "${cmd}"; sleep 3; printf 'quit\r'; sleep 1; } \
        | nc -w 8 127.0.0.1 ${TELNET_PORT} 2>/dev/null
}

# Warm-up: the first connection's banner/readline init can eat early input —
# burn one session so command 1 doesn't flake.
send_cmd "" >/dev/null

echo "smoke: running command battery ..."
while IFS='|' read -r cmd want; do
    [ -z "${cmd}" ] && continue
    out="$(send_cmd "${cmd}")"
    printf '%s\n' "${out}" >>"${OUT}/battery.log"
    if printf '%s' "${out}" | grep -qi "${want}"; then
        PASS=$((PASS + 1)); echo "  PASS  ${cmd}"
    else
        FAIL=$((FAIL + 1)); echo "  FAIL  ${cmd}   (want '${want}')"
    fi
done <<< "${BATTERY}"

# --- archive artifacts ----------------------------------------------------
cp "${SERIAL_LOG}" "${OUT}/serial.log" 2>/dev/null
cp "${PCAP}" "${OUT}/net.pcap" 2>/dev/null

echo
echo "smoke: ${PASS} passed, ${FAIL} failed"
echo "smoke: artifacts in ${OUT}"

if [ ${FAIL} -gt 0 ]; then
    echo "smoke: serial tail:"
    tail -30 "${SERIAL_LOG}" 2>/dev/null
    exit 1
fi
exit 0
