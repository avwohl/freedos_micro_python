#!/usr/bin/env bash
# MicroPython networking smoke under FreeDOS + QEMU with NE2000.COM
# loaded at INT 0x60 (the Crynwr packet-driver TSR convention every
# mTCP-targeting DOS app uses). This proves MP talks via Crynwr —
# any NIC with a Crynwr driver should work the same way under real
# FreeDOS.
#
# Steps:
#   1. Build a FreeDOS floppy with NE2000.COM + MP.EXE + NETBASE.PY.
#   2. AUTOEXEC loads NE2000 0x60 9 0x300, runs MP.EXE.
#   3. Pipe NETBASE.PY (paste-mode) over COM1. It calls
#      uc386_net.eth_init(True) for DHCP and prints eth_status().
#   4. PASS if eth_status reports up=True and a non-zero IP (the
#      SLIRP DHCP server hands out 10.0.2.15 by convention).

set -eu

cd "$(dirname "$0")"

MP_EXE="${1:-../dosbox-x-rig/MP.EXE}"
NE2000_COM="${NE2000_COM:-../dosbox-x-rig/NE2000.COM}"

for f in "$MP_EXE" "$NE2000_COM"; do
    if [ ! -f "$f" ]; then
        echo "missing required staging file: $f" >&2
        exit 1
    fi
done

FREEDOS_IMG=/tmp/freedos.img
TEST_IMG="$(pwd)/freedos-netbase.img"
LOG="$(pwd)/qemu-netbase.log"
PCAP="$(pwd)/qemu-netbase.pcap"

if [ ! -f "$FREEDOS_IMG" ]; then
    echo "[rig] fetching FreeDOS boot floppy ..."
    curl -fsSL -o "$FREEDOS_IMG" \
        https://raw.githubusercontent.com/codercowboy/freedosbootdisks/master/bootdisks/freedos.boot.disk.1.4MB.img
fi

echo "[rig] building FAT12 image ..."
cp "$FREEDOS_IMG" "$TEST_IMG"
mcopy -i "$TEST_IMG" -o "$NE2000_COM" ::NE2000.COM
mcopy -i "$TEST_IMG" -o "$MP_EXE"     ::MP.EXE
mcopy -i "$TEST_IMG" -o ./NETBASE.PY  ::NETBASE.PY

NETBASE_WRAPPED="$(pwd)/netbase-wrapped.in"
{
    printf '\x05'
    cat ./NETBASE.PY
    printf '\x04'
    printf '\n\x04'
} > "$NETBASE_WRAPPED"

AUTOEXEC=$(mktemp)
{
    printf '@echo off\r\n'
    printf 'CTTY COM1\r\n'
    printf 'echo === netbase rig ===\r\n'
    printf 'NE2000 0x60 9 0x300\r\n'
    printf 'echo --- pktdrv loaded ---\r\n'
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
    -object filter-dump,id=f0,netdev=net0,file="$PCAP" \
    -no-reboot \
    < "$NETBASE_WRAPPED" > "$LOG" 2>&1 &
QEMU_PID=$!

for _ in $(seq 1 120); do
    sleep 1
    if grep -q "NETBASE: PASS\|NETBASE: FAIL\|rig done" "$LOG" 2>/dev/null; then
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
echo "=== Captured COM1 (qemu-netbase.log) ==="
cat "$LOG"

echo
echo "=== Result ==="
if tr -d '\r' < "$LOG" | grep -qE '^NETBASE: PASS$' 2>/dev/null; then
    echo "PASS: MP+Crynwr packet driver works under standard FreeDOS."
    exit 0
fi

echo "Progress markers seen:"
for marker in "NETBASE: start" \
              "NETBASE: pre_eth_init" \
              "NETBASE: eth_init" \
              "NETBASE: ip="; do
    if tr -d '\r' < "$LOG" | grep -qE "^${marker}" 2>/dev/null; then
        echo "  YES: ${marker}"
    fi
done

echo "FAIL: MP did not establish network via Crynwr packet driver."
exit 1
