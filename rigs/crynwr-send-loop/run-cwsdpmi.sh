#!/usr/bin/env bash
# Same bench as run.sh but loads CWSDPMI as the DPMI host BEFORE
# DOS/32A starts. Per DOS/32A's docs, it can defer to an external
# DPMI host. CWSDPMI is the gold-standard DPMI 0.9 implementation
# used by DJGPP+Watt32 for decades; if our AH=04 send_pkt GPF goes
# away under CWSDPMI, the bug is confirmed as DOS/32A-specific and
# we have a clean alternative.
#
# Pair with build using `NO_IRQ_MASK=1 ./build.sh` so the bench
# doesn't mask the NIC IRQ — otherwise we can't tell whether
# CWSDPMI or our mask is what kept it alive.
set -eu

cd "$(dirname "$0")"

CRYN_EXE="${1:-./build/CRYN.EXE}"
if [ ! -f "$CRYN_EXE" ]; then
    echo "CRYN.EXE not found — run NO_IRQ_MASK=1 ./build.sh first" >&2
    exit 1
fi

NE2000_COM="${NE2000_COM:-../dosbox-x-rig/NE2000.COM}"
CWSDPMI_EXE="${CWSDPMI_EXE:-../dosbox-x-rig/CWSDPMI.EXE}"
for f in "$NE2000_COM" "$CWSDPMI_EXE"; do
    if [ ! -f "$f" ]; then
        echo "missing required staging file: $f" >&2
        exit 1
    fi
done

FREEDOS_IMG=/tmp/freedos.img
TEST_IMG="$(pwd)/freedos-cwsdpmi.img"
LOG="$(pwd)/qemu-cwsdpmi.log"

cp "$FREEDOS_IMG" "$TEST_IMG"
mcopy -i "$TEST_IMG" -o "$CWSDPMI_EXE" ::CWSDPMI.EXE
mcopy -i "$TEST_IMG" -o "$NE2000_COM" ::NE2000.COM
mcopy -i "$TEST_IMG" -o "$CRYN_EXE" ::CRYN.EXE

AUTOEXEC=$(mktemp)
{
    printf '@echo off\r\n'
    printf 'CTTY COM1\r\n'
    printf 'echo === crynwr / CWSDPMI rig ===\r\n'
    # Load CWSDPMI as a persistent DPMI host. Naked invocation
    # TSRs per the v7b docs ("cwsdpmi alone with no parameters
    # will terminate and stay resident").
    printf 'CWSDPMI\r\n'
    printf 'echo --- cwsdpmi loaded ---\r\n'
    # Tell DOS/32A to use external DPMI. DPMITST is deprecated
    # per the v9.1.2 changelog, but the relevant working knob is
    # NOVCPI / NOXMS which forces it through DPMI detection. Try
    # the simplest first: just load CWSDPMI and let DOS/32A's
    # default detection find it.
    printf 'SET DOS32A=NOVCPI NOXMS\r\n'
    # Crynwr NE2000 packet driver at INT 0x60.
    printf 'NE2000 0x60 9 0x300\r\n'
    printf 'echo --- ne2000 loaded ---\r\n'
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

echo "[rig] booting QEMU + FreeDOS + CWSDPMI + NE2000 ..."
qemu-system-i386 \
    -display none \
    -serial stdio \
    -fda "$TEST_IMG" \
    -boot a \
    -m 16 \
    -cpu pentium \
    -netdev user,id=net0 \
    -device ne2k_isa,netdev=net0,iobase=0x300,irq=9 \
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
