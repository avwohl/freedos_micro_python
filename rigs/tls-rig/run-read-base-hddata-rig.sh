#!/usr/bin/env bash
# Boot FreeDOS from -fda (same as before), but place TESTCA.PEM on a
# SECOND drive: a properly-partitioned 10 MB FAT12 HDD image attached
# via -hda. AUTOEXEC.BAT switches to C: before launching MP.EXE so
# the open('TESTCA.PEM', ...) call resolves to the HDD file, not the
# floppy. If the read succeeds from C: but hangs from A:, the floppy
# driver path is the bug.

set -eu

cd "$(dirname "$0")"

MP_EXE="${1:-../dosbox-x-rig/MP.EXE}"

if [ ! -f "$MP_EXE" ]; then
    echo "MP.EXE not found at $MP_EXE" >&2
    exit 1
fi

FREEDOS_IMG=/tmp/freedos.img
TEST_IMG="$(pwd)/freedos-readbase-hddata.img"
HDD_IMG=/tmp/hdd_test.img
LOG="$(pwd)/qemu-readbase-hddata.log"

if [ ! -f "$FREEDOS_IMG" ]; then
    curl -fsSL -o "$FREEDOS_IMG" \
        https://raw.githubusercontent.com/codercowboy/freedosbootdisks/master/bootdisks/freedos.boot.disk.1.4MB.img
fi

# Make sure HDD image with C:TESTCA.PEM exists (built externally).
if [ ! -f "$HDD_IMG" ]; then
    echo "HDD image $HDD_IMG missing — build it first." >&2
    exit 1
fi

if [ ! -f test-server-ca.pem ]; then
    openssl req -x509 -nodes -newkey rsa:1024 \
        -subj "/CN=readbase-placeholder" -days 1 \
        -keyout /tmp/_rb.key -out /tmp/_rb.crt 2>/dev/null
    cp /tmp/_rb.crt test-server-ca.pem
    rm -f /tmp/_rb.key /tmp/_rb.crt
fi

echo "[rig] building boot floppy (MP + script, NO TESTCA.PEM) ..."
cp "$FREEDOS_IMG" "$TEST_IMG"
mcopy -i "$TEST_IMG" -o "$MP_EXE"      ::MP.EXE
mcopy -i "$TEST_IMG" -o ./READBASE.PY  ::READBASE.PY

READBASE_WRAPPED="$(pwd)/readbase-hddata-wrapped.in"
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
    printf 'echo === read-base-hddata rig (read from HDD C:) ===\r\n'
    # Switch to C: before running MP so open('TESTCA.PEM') hits the
    # HDD. Also copy READBASE.PY across since paste-mode feeds it.
    printf 'C:\r\n'
    printf 'echo === cwd is now C: ===\r\n'
    printf 'A:\\MP.EXE\r\n'
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

echo "[rig] booting QEMU -fda (boot) + -hda (data) ..."
qemu-system-i386 \
    -display none \
    -serial stdio \
    -fda "$TEST_IMG" \
    -drive file="$HDD_IMG",format=raw,if=ide,index=0,media=disk \
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
echo "=== Captured COM1 (qemu-readbase-hddata.log) ==="
cat "$LOG"

echo
echo "=== Result ==="
if grep -qE '^READBASE: PASS$' "$LOG" 2>/dev/null; then
    echo "PASS: f.read() works from HDD C: — floppy driver path is the bug."
    exit 0
fi

echo "Progress markers seen:"
for marker in "cwd is now C:" \
              "READBASE: start" \
              "READBASE: pre_open" \
              "READBASE: post_open" \
              "READBASE: pre_read"; do
    if grep -q "${marker}" "$LOG" 2>/dev/null; then
        echo "  YES: ${marker}"
    fi
done
if grep -qE '^READBASE: post_read' "$LOG" 2>/dev/null; then
    echo "  YES: READBASE: post_read"
fi

echo "FAIL"
exit 1
