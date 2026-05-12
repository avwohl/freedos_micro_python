#!/usr/bin/env bash
# MicroPython networking smoke under dosiz with its built-in virtual
# Crynwr packet driver (INT 0x60). The companion run-net-base-rig.sh
# does the same thing under QEMU+FreeDOS+NE2000.COM — between the two,
# they prove MP works against the Crynwr packet-driver interface in
# both standard FreeDOS and dosiz.
#
# Steps:
#   1. Stage MP.EXE + NETBASE.PY into a temp dir.
#   2. Build a minimal dosiz .cfg pointing at MP.EXE with that dir as
#      drive C:.
#   3. Pipe NETBASE.PY (paste-mode) over stdin. The script imports
#      uc386_net, calls eth_init(True) for DHCP, and prints status.
#   4. PASS if eth_status reports a non-zero IP. SLIRP hands out
#      10.0.2.15 by convention.

set -eu

cd "$(dirname "$0")"

MP_EXE="${1:-../dosbox-x-rig/MP.EXE}"
DOSIZ_BIN="${DOSIZ_BIN:-$HOME/src/dosiz/build/dosiz}"

for f in "$MP_EXE" "$DOSIZ_BIN"; do
    if [ ! -f "$f" ]; then
        echo "missing required file: $f" >&2
        exit 1
    fi
done

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT INT TERM

cp "$MP_EXE"      "$WORKDIR/MP.EXE"
cp ./NETBASE.PY   "$WORKDIR/NETBASE.PY"

cat > "$WORKDIR/dosiz.cfg" <<EOF
program        = $WORKDIR/MP.EXE
drive_C        = $WORKDIR
default_mode   = binary
EOF

NETBASE_WRAPPED="$WORKDIR/netbase-wrapped.in"
{
    printf '\x05'
    cat ./NETBASE.PY
    printf '\x04'
    printf '\n\x04'
} > "$NETBASE_WRAPPED"

LOG="$(pwd)/dosiz-netbase.log"

echo "[rig] launching dosiz ..."
timeout 30 "$DOSIZ_BIN" "$WORKDIR/dosiz.cfg" \
    < "$NETBASE_WRAPPED" > "$LOG" 2>&1 || true

echo
echo "=== Captured stdout (dosiz-netbase.log) ==="
cat "$LOG"

echo
echo "=== Result ==="
if tr -d '\r' < "$LOG" | grep -qE '^NETBASE: PASS$' 2>/dev/null; then
    echo "PASS: MP+Crynwr packet driver works under dosiz."
    exit 0
fi

echo "Progress markers seen:"
for marker in "NETBASE: start" \
              "NETBASE: eth_init" \
              "NETBASE: thunk_inv" \
              "NETBASE: ip="; do
    if tr -d '\r' < "$LOG" | grep -qE "^${marker}" 2>/dev/null; then
        echo "  YES: ${marker}"
    fi
done

echo "FAIL: MP did not establish network via dosiz's INT 0x60 Crynwr."
exit 1
