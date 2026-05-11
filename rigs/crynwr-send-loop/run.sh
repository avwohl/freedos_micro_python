#!/usr/bin/env bash
# Run CRYN.EXE under FreeDOS+QEMU+ne2k_isa with NE2000.COM loaded
# at INT 0x60 first.
set -eu

cd "$(dirname "$0")"

CRYN_EXE="${1:-./build/CRYN.EXE}"
if [ ! -f "$CRYN_EXE" ]; then
    echo "CRYN.EXE not found — run ./build.sh first" >&2
    exit 1
fi

NE2000_COM="${NE2000_COM:-../dosbox-x-rig/NE2000.COM}"
if [ ! -f "$NE2000_COM" ]; then
    echo "NE2000.COM not found at $NE2000_COM" >&2
    exit 1
fi

FREEDOS_IMG=/tmp/freedos.img
TEST_IMG="$(pwd)/freedos-cryn.img"
LOG="$(pwd)/qemu-cryn.log"

if [ ! -f "$FREEDOS_IMG" ]; then
    curl -fsSL -o "$FREEDOS_IMG" \
        https://raw.githubusercontent.com/codercowboy/freedosbootdisks/master/bootdisks/freedos.boot.disk.1.4MB.img
fi

cp "$FREEDOS_IMG" "$TEST_IMG"
mcopy -i "$TEST_IMG" -o "$NE2000_COM" ::NE2000.COM
mcopy -i "$TEST_IMG" -o "$CRYN_EXE" ::CRYN.EXE

AUTOEXEC=$(mktemp)
{
    printf '@echo off\r\n'
    printf 'CTTY COM1\r\n'
    printf 'echo === crynwr-send-loop rig ===\r\n'
    printf 'NE2000 0x60 9 0x300\r\n'
    printf 'echo --- pktdrv loaded ---\r\n'
    printf 'CRYN.EXE\r\n'
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

echo "[rig] booting QEMU + FreeDOS + NE2000 ..."
qemu-system-i386 \
    -display none \
    -serial stdio \
    -fda "$TEST_IMG" \
    -boot a \
    -m 16 \
    -cpu pentium \
    -netdev user,id=net0 \
    -device ne2k_isa,netdev=net0,iobase=0x300,irq=9 \
    -object filter-dump,id=f0,netdev=net0,file="$(pwd)/qemu-cryn.pcap" \
    -no-reboot \
    > "$LOG" 2>&1 &
QEMU_PID=$!

# Three-minute cap; if we haven't reached [bench:done] or a GPF by
# then, kill it.
for _ in $(seq 1 180); do
    sleep 1
    if grep -q "bench:done\|GPF\|exception (INT\|rig done" "$LOG" 2>/dev/null; then
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
echo "=== Captured COM1 ==="
cat "$LOG"
echo
if grep -q "bench:done" "$LOG" 2>/dev/null; then
    echo "PASS"
    exit 0
fi
echo "FAIL"
exit 1
