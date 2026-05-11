#!/usr/bin/env bash
# Run /tmp/dpmi_int21_smoke/DPMI.EXE under FreeDOS+QEMU. Fast loop:
# rebuild .exe in 0.3 sec via pyle, rerun the rig in ~10 sec.
set -eu

cd "$(dirname "$0")"

DPMI_EXE="${1:-./DPMI.EXE}"
if [ ! -f "$DPMI_EXE" ]; then
    echo "DPMI.EXE not found at $DPMI_EXE" >&2
    exit 1
fi

FREEDOS_IMG=/tmp/freedos.img
TEST_IMG="$(pwd)/freedos-dpmi.img"
LOG="$(pwd)/qemu-dpmi.log"

if [ ! -f "$FREEDOS_IMG" ]; then
    curl -fsSL -o "$FREEDOS_IMG" \
        https://raw.githubusercontent.com/codercowboy/freedosbootdisks/master/bootdisks/freedos.boot.disk.1.4MB.img
fi

# A short placeholder for TESTCA.PEM — content doesn't matter, just
# needs to exist on the FAT12 image so AH=3D succeeds.
if [ ! -f testca.pem ]; then
    printf 'BEGIN-CERT-MARKER-1234567890ABC\n' > testca.pem
fi

echo "[rig] building boot floppy ..."
cp "$FREEDOS_IMG" "$TEST_IMG"
mcopy -i "$TEST_IMG" -o "$DPMI_EXE" ::DPMI.EXE
mcopy -i "$TEST_IMG" -o ./testca.pem ::TESTCA.PEM

AUTOEXEC=$(mktemp)
{
    printf '@echo off\r\n'
    printf 'CTTY COM1\r\n'
    printf 'echo === dpmi-int21 smoke ===\r\n'
    printf 'DPMI.EXE\r\n'
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
    if grep -q "smoke:PASS\|smoke:.*FAIL\|rig done" "$LOG" 2>/dev/null; then
        sleep 1
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
echo "=== Captured COM1 ==="
cat "$LOG"
echo
if grep -q "smoke:PASS" "$LOG" 2>/dev/null; then
    echo "PASS"
    exit 0
fi
echo "FAIL"
exit 1
