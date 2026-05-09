#!/usr/bin/env bash
# Repro the "MP.EXE under QEMU + FreeDOS" test that the mp-rig CI
# workflow runs. Boots a FreeDOS minimal floppy under QEMU with
# the host's stdout wired to the guest's COM1 (via DOS CTTY), runs
# the bridge stub + MP.EXE, and stops as soon as the MicroPython
# REPL banner appears.
#
# This is the closest local-machine proxy for the real production
# target (FreeDOS in VMware): QEMU emulates a full PC, FreeDOS is
# the actual DOS kernel, and PMODE/W's INT 21h reflection talks
# to that FreeDOS — the same path the production VM uses.
#
# Prereqs:
#   - qemu-system-i386 (brew install qemu / apt install qemu-system-x86)
#   - mtools           (brew install mtools / apt install mtools)
#   - MP.EXE built via addons/harness/exe.py (see ../README.md)
#   - ECHOTEST.EXE built from addons/gnu/echo (optional, baseline)
#
# Usage:
#   ./run-qemu-freedos.sh             # uses ./MP.EXE in this dir
#   ./run-qemu-freedos.sh /path/to/MP.EXE
#
# The captured log is written to qemu-freedos.log next to this script.

set -eu

cd "$(dirname "$0")"

MP_EXE="${1:-./MP.EXE}"
if [ ! -f "$MP_EXE" ]; then
    echo "MP.EXE not found at $MP_EXE — build it via:" >&2
    echo "  python -m addons.harness.exe \\" >&2
    echo "    addons/gnu/micropython/build/micropython.bin \\" >&2
    echo "    -o addons/gnu/micropython/dosbox-x-rig/MP.EXE" >&2
    exit 1
fi

FREEDOS_IMG=/tmp/freedos.img
TEST_IMG=/tmp/freedos-mp.img
LOG=./qemu-freedos.log

# Fetch the FreeDOS minimal boot floppy if we don't have it. The
# codercowboy mirror is convenient and stable; the upstream FreeDOS
# project also distributes equivalent floppy images.
if [ ! -f "$FREEDOS_IMG" ]; then
    echo "Fetching FreeDOS boot floppy ..."
    curl -fsSL -o "$FREEDOS_IMG" \
        https://raw.githubusercontent.com/codercowboy/freedosbootdisks/master/bootdisks/freedos.boot.disk.1.4MB.img
fi

# Build the test floppy: copy MP.EXE + a tiny AUTOEXEC.BAT onto a
# fresh copy of the FreeDOS image. AUTOEXEC redirects DOS console
# I/O to COM1 so QEMU's -serial stdio captures it.
cp "$FREEDOS_IMG" "$TEST_IMG"
mcopy -i "$TEST_IMG" -o "$MP_EXE" ::MP.EXE
[ -f ./ECHOTEST.EXE ] && mcopy -i "$TEST_IMG" -o ./ECHOTEST.EXE ::ECHOTEST.EXE || true

# CRLF line endings — DOS expects them. No `<` or `>` redirects on
# MP.EXE: MP's REPL reads via INT 21h AH=0x01 (CON character I/O)
# which doesn't honor file-handle redirects after CTTY COM1, and
# the goal is just to show that the full _main → _write path runs.
{
    printf '@echo off\r\n'
    printf 'CTTY COM1\r\n'
    printf 'echo === FreeDOS rig start ===\r\n'
    if [ -f ./ECHOTEST.EXE ]; then
        printf 'ECHOTEST.EXE hello qemu freedos\r\n'
    fi
    printf 'echo === before mp ===\r\n'
    printf 'MP.EXE\r\n'
    printf 'echo === finished ===\r\n'
} > /tmp/AUTOEXEC.BAT
mcopy -i "$TEST_IMG" -o /tmp/AUTOEXEC.BAT ::AUTOEXEC.BAT

# Boot QEMU headless with COM1 piped to host stdout. Stop as soon
# as the MicroPython banner appears (MP doesn't exit cleanly under
# headless serial). Hard cap at 180s.
echo "Booting QEMU + FreeDOS (log: $LOG) ..."
qemu-system-i386 \
    -display none \
    -serial stdio \
    -fda "$TEST_IMG" \
    -boot a \
    -m 16 \
    -cpu pentium \
    -no-reboot \
    < /dev/null > "$LOG" 2>&1 &
QEMU_PID=$!

trap 'kill -KILL $QEMU_PID 2>/dev/null || true' EXIT

for _ in $(seq 1 180); do
    sleep 1
    if grep -q "MicroPython" "$LOG" 2>/dev/null; then
        # Give MP a beat to finish printing the banner + REPL prompt.
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
echo "=== Captured COM1 output ==="
cat "$LOG"
echo
echo "=== Result ==="
if grep -q "MicroPython" "$LOG"; then
    echo "PASS: MicroPython REPL banner found — full _main → _write path works"
else
    echo "MISS: no MicroPython banner — see $LOG for details"
    exit 1
fi
