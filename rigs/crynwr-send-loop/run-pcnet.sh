#!/usr/bin/env bash
# Same bench as run.sh but with QEMU's AMD PCnet (`-device pcnet`)
# and the Crynwr `pcntpk.com` packet driver instead of NE2000.
#
# Purpose: the bench under NE2000.COM + ne2k_isa GPFs on AH=04 send_pkt
# in an identical-signature way across many parameter sweeps. PCnet is
# a completely different chip family (AMD LANCE), driven by a
# completely different Crynwr driver. If THIS rig completes a send,
# the AH=04 bug is NE2000.COM-specific and we're done — the port is
# proven to work over any non-NE2000 Crynwr-supported NIC. If THIS
# also GPFs in DOS/32A's cleanup, the bug is in DOS/32A's RM
# transition path for ANY INT-handled NIC send.
set -eu

cd "$(dirname "$0")"

CRYN_EXE="${1:-./build/CRYN.EXE}"
if [ ! -f "$CRYN_EXE" ]; then
    echo "CRYN.EXE not found — run ./build.sh first" >&2
    exit 1
fi

PCNTPK_COM="${PCNTPK_COM:-../dosbox-x-rig/pcntpk.com}"
if [ ! -f "$PCNTPK_COM" ]; then
    echo "pcntpk.com not found at $PCNTPK_COM — fetch via:" >&2
    echo "  curl -fsSL https://archive.org/download/lan-packet-drivers-for-ms-dos/drvlan/pcntpk.zip | unzip -p - pcntpk.com > $PCNTPK_COM" >&2
    exit 1
fi

FREEDOS_IMG=/tmp/freedos.img
TEST_IMG="$(pwd)/freedos-pcnet.img"
LOG="$(pwd)/qemu-pcnet.log"

cp "$FREEDOS_IMG" "$TEST_IMG"
mcopy -i "$TEST_IMG" -o "$PCNTPK_COM" ::PCNTPK.COM
mcopy -i "$TEST_IMG" -o "$CRYN_EXE" ::CRYN.EXE

AUTOEXEC=$(mktemp)
{
    printf '@echo off\r\n'
    printf 'CTTY COM1\r\n'
    printf 'echo === crynwr-send-loop / PCnet rig ===\r\n'
    # Crynwr pcntpk uses keyword args, not positional. Bind on
    # INT 0x60 so pktdrv_uc386dos.c's IVT scan finds it (same slot
    # the NE2000 rig uses).
    printf 'PCNTPK INT=0x60\r\n'
    printf 'echo --- pcntpk loaded ---\r\n'
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

echo "[rig] booting QEMU + FreeDOS + AMD PCnet ..."
qemu-system-i386 \
    -display none \
    -serial stdio \
    -fda "$TEST_IMG" \
    -boot a \
    -m 16 \
    -cpu pentium \
    -netdev user,id=net0 \
    -device pcnet,netdev=net0 \
    -object filter-dump,id=f0,netdev=net0,file="$(pwd)/qemu-pcnet.pcap" \
    -no-reboot \
    > "$LOG" 2>&1 &
QEMU_PID=$!

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
