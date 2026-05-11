#!/usr/bin/env bash
# Run BIGINT.EXE under FreeDOS+QEMU and collect the bench output.
# Mirrors rigs/dpmi-int21-smoke/run.sh shape.
set -eu

cd "$(dirname "$0")"

BIGINT_EXE="${1:-./build/BIGINT.EXE}"
if [ ! -f "$BIGINT_EXE" ]; then
    echo "BIGINT.EXE not found at $BIGINT_EXE — run ./build.sh first" >&2
    exit 1
fi

FREEDOS_IMG=/tmp/freedos.img
TEST_IMG="$(pwd)/freedos-bigint.img"
LOG="$(pwd)/qemu-bigint.log"

if [ ! -f "$FREEDOS_IMG" ]; then
    curl -fsSL -o "$FREEDOS_IMG" \
        https://raw.githubusercontent.com/codercowboy/freedosbootdisks/master/bootdisks/freedos.boot.disk.1.4MB.img
fi

cp "$FREEDOS_IMG" "$TEST_IMG"
mcopy -i "$TEST_IMG" -o "$BIGINT_EXE" ::BIGINT.EXE

AUTOEXEC=$(mktemp)
{
    printf '@echo off\r\n'
    printf 'CTTY COM1\r\n'
    printf 'echo === bigint bench ===\r\n'
    printf 'BIGINT.EXE\r\n'
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

echo "[rig] booting QEMU + FreeDOS ..."
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

# Wait up to 5 minutes for `[bench:done]` or `rig done`.
for _ in $(seq 1 300); do
    sleep 1
    if grep -q "bench:done\|rig done" "$LOG" 2>/dev/null; then
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
if grep -q "bench:done" "$LOG" 2>/dev/null; then
    echo "PASS"
    exit 0
fi
echo "FAIL"
exit 1
