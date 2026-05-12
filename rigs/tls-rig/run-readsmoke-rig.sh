#!/usr/bin/env bash
# Run /tmp/readsmoke/READ.EXE (a 30-line PM client) under FreeDOS+QEMU.
# Same scaffolding as run-read-base-rig.sh but with the smoke test
# in place of MP.EXE. Goal: isolate whether the disk-read hang is
# in MicroPython's heap-management interaction with extender state,
# or universal across any PM client built through uc386 + pyle +
# PMODE/W.

set -eu

cd "$(dirname "$0")"

READ_EXE="${1:-/tmp/readsmoke/READ.EXE}"

if [ ! -f "$READ_EXE" ]; then
    echo "READ.EXE not found at $READ_EXE" >&2
    exit 1
fi

FREEDOS_IMG=/tmp/freedos.img
TEST_IMG="$(pwd)/freedos-readsmoke.img"
LOG="$(pwd)/qemu-readsmoke.log"

if [ ! -f "$FREEDOS_IMG" ]; then
    curl -fsSL -o "$FREEDOS_IMG" \
        https://raw.githubusercontent.com/codercowboy/freedosbootdisks/master/bootdisks/freedos.boot.disk.1.4MB.img
fi

if [ ! -f test-server-ca.pem ]; then
    openssl req -x509 -nodes -newkey rsa:1024 \
        -subj "/CN=readbase-placeholder" -days 1 \
        -keyout /tmp/_rb.key -out /tmp/_rb.crt 2>/dev/null
    cp /tmp/_rb.crt test-server-ca.pem
    rm -f /tmp/_rb.key /tmp/_rb.crt
fi

echo "[rig] building FAT12 image ..."
cp "$FREEDOS_IMG" "$TEST_IMG"
mcopy -i "$TEST_IMG" -o "$READ_EXE" ::READ.EXE
mcopy -i "$TEST_IMG" -o ./test-server-ca.pem ::TESTCA.PEM

AUTOEXEC=$(mktemp)
{
    printf '@echo off\r\n'
    printf 'CTTY COM1\r\n'
    printf 'echo === readsmoke rig ===\r\n'
    printf 'READ.EXE\r\n'
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

echo "[rig] booting QEMU + FreeDOS (no NIC, no MP) ..."
qemu-system-i386 \
    -display none \
    -serial stdio \
    -fda "$TEST_IMG" \
    -boot a \
    -m 16 \
    -cpu pentium \
    -no-reboot \
    > "$LOG" 2>&1 &
QEMU_PID=$!

for _ in $(seq 1 30); do
    sleep 1
    if grep -q "smoke:PASS\|smoke:open-FAIL\|rig done" "$LOG" 2>/dev/null; then
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
echo "=== Captured COM1 (qemu-readsmoke.log) ==="
cat "$LOG"

echo
echo "=== Result ==="
if grep -q "smoke:PASS" "$LOG" 2>/dev/null; then
    echo "PASS: tiny PM smoke test completed — disk read works without MP."
    echo "       The hang is something MicroPython does before/around the read."
    exit 0
fi
if grep -q "smoke:read-returned" "$LOG" 2>/dev/null; then
    echo "READ RETURNED — but didn't reach PASS. Investigate further."
    exit 1
fi
if grep -q "smoke:pre-read" "$LOG" 2>/dev/null; then
    echo "HUNG inside read() — universal uc386+libc+extender issue, NOT MP-specific."
    exit 1
fi
echo "FAIL: didn't reach pre-read marker"
exit 1
