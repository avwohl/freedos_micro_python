#!/usr/bin/env bash
# Baseline f.read() test — NO eth_init, NO lwIP, NO network at all.
# Goal: isolate whether f.read() hangs in isolation, or only after
# eth_init disturbs DPMI/PMODE/W state. Same scaffolding as
# run-read-rig.sh but feeds READBASE.PY instead.

set -eu

cd "$(dirname "$0")"

MP_EXE="${1:-../dosbox-x-rig/MP.EXE}"

if [ ! -f "$MP_EXE" ]; then
    echo "MP.EXE not found at $MP_EXE" >&2
    exit 1
fi

FREEDOS_IMG=/tmp/freedos.img
TEST_IMG="$(pwd)/freedos-readbase.img"
LOG="$(pwd)/qemu-readbase.log"

if [ ! -f "$FREEDOS_IMG" ]; then
    echo "[rig] fetching FreeDOS boot floppy ..."
    curl -fsSL -o "$FREEDOS_IMG" \
        https://raw.githubusercontent.com/codercowboy/freedosbootdisks/master/bootdisks/freedos.boot.disk.1.4MB.img
fi

if [ ! -f test-server-ca.pem ]; then
    echo "[rig] generating placeholder TESTCA.PEM ..."
    openssl req -x509 -nodes -newkey rsa:1024 \
        -subj "/CN=readbase-placeholder" \
        -days 1 \
        -keyout /tmp/_rb.key -out /tmp/_rb.crt 2>/dev/null
    cp /tmp/_rb.crt test-server-ca.pem
    rm -f /tmp/_rb.key /tmp/_rb.crt
fi

echo "[rig] building FAT12 image ..."
cp "$FREEDOS_IMG" "$TEST_IMG"
mcopy -i "$TEST_IMG" -o "$MP_EXE"      ::MP.EXE
mcopy -i "$TEST_IMG" -o ./READBASE.PY  ::READBASE.PY
mcopy -i "$TEST_IMG" -o ./test-server-ca.pem ::TESTCA.PEM

READBASE_WRAPPED="$(pwd)/readbase-wrapped.in"
{
    printf '\x05'
    cat ./READBASE.PY
    printf '\x04'
    printf '\n\x04'
} > "$READBASE_WRAPPED"

AUTOEXEC=$(mktemp)
{
    printf '@echo off\r\n'
    printf 'CTTY COM1\r\n'
    printf 'echo === read-base rig (no net) ===\r\n'
    printf 'MP.EXE\r\n'
    printf 'echo === rig done ===\r\n'
} > "$AUTOEXEC"
mcopy -i "$TEST_IMG" -o "$AUTOEXEC" ::AUTOEXEC.BAT
rm -f "$AUTOEXEC"

cleanup() {
    rc=$?
    [ -n "${QEMU_PID:-}" ] && kill -KILL "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    return $rc
}
trap cleanup EXIT INT TERM

echo "[rig] booting QEMU + FreeDOS (no NIC at all) ..."
# No NE2000 — pure DOS, just MP + a file to read.
qemu-system-i386 \
    -display none \
    -serial stdio \
    -fda "$TEST_IMG" \
    -boot a \
    -m 16 \
    -cpu pentium \
    -no-reboot \
    < "$READBASE_WRAPPED" > "$LOG" 2>&1 &
QEMU_PID=$!

for _ in $(seq 1 60); do
    sleep 1
    if grep -q "READBASE: PASS\|READBASE: FAIL\|rig done" "$LOG" 2>/dev/null; then
        sleep 2
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        break
    fi
done
kill "$QEMU_PID" 2>/dev/null || true
sleep 1
kill -KILL "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

echo
echo "=== Captured COM1 (qemu-readbase.log) ==="
cat "$LOG"

echo
echo "=== Result ==="
# CTTY COM1 emits CRLF, so anchored `$` regexes need to see \r as
# whitespace — strip CRs before matching.
if tr -d '\r' < "$LOG" | grep -qE '^READBASE: PASS$' 2>/dev/null; then
    echo "PASS: f.read() works without eth_init — eth_init disturbs DPMI state."
    exit 0
fi

echo "Progress markers seen:"
for marker in "READBASE: start" \
              "READBASE: pre_open" \
              "READBASE: post_open" \
              "READBASE: pre_read"; do
    if grep -qE "^${marker}" "$LOG" 2>/dev/null; then
        echo "  YES: ${marker}"
    fi
done
# post_read uses anchored prefix match to dodge the paste-mode source echo.
if grep -qE '^READBASE: post_read' "$LOG" 2>/dev/null; then
    echo "  YES: READBASE: post_read"
fi

echo "FAIL: f.read() hangs without eth_init — baseline broken, not network-induced."
exit 1
