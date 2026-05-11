#!/usr/bin/env bash
# Same bench as run.sh but loads CWSDPMI as the DPMI host BEFORE
# DOS/32A starts (via SET DOS32A=NOVCPI NOXMS). CWSDPMI is the
# gold-standard DPMI 0.9 host used by DJGPP+Watt32 for decades.
#
# FINDING (kept as a documented alternative host, not a fix):
#   - Default build (IRQ mask ON): bench PASSes end-to-end under
#     CWSDPMI — 5/5 sends OK, reaches [bench:done]. Confirms our
#     stack is portable across DPMI hosts.
#   - NO_IRQ_MASK=1 build: reproduces the same AH=04 send_pkt
#     GPF under CWSDPMI that we see on DOS/32A's native server.
#     So the GPF is in the real-mode IRQ→PM transition (NIC IRQ
#     firing during the RM send_pkt), not DOS/32A-specific.
#     The IRQ-mask workaround in pktdrv_init is required either
#     way; CWSDPMI does not sidestep it.
#
# Prereq: build.sh produced build/CRYN.EXE. The bridge stub
# (uc386 addons/harness/exe.py) had its early-2024 diagnostic
# dereferences stripped — those touched `[_main + N]` and PF'd
# under CWSDPMI's stricter paging. See exe.py near the
# `--- LE FIXUP / runtime addressing diagnostics ---` comment.
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
