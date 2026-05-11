#!/usr/bin/env bash
# Run TLS.EXE via the standard FreeDOS-packet-driver path that
# mTCP / htget use: load NE2000.COM at INT 0x60 first, then run our
# .EXE, which (built with FORCE_CRYNWR=1) skips its PM-native NE2000
# fast path and goes through INT 0x60 + DPMI 0x0303 instead.
#
# If this rig completes the same handshake stages the QEMU path
# reaches (TCP-connected, ServerHello, Cert, SHD) without hangs in
# pktdrv_init or pktdrv_recv, the conclusion is that the port can
# ride on top of any DOS packet driver — no PM-native LANCE/3C509/
# RTL8139 driver work needed.
set -eu

cd "$(dirname "$0")"

TLS_EXE="${1:-./build/TLS.EXE}"
TLS_PORT="${TLS_PORT:-8443}"

if [ ! -f "$TLS_EXE" ]; then
    echo "TLS.EXE not found at $TLS_EXE — run 'FORCE_CRYNWR=1 ./build.sh' first" >&2
    exit 1
fi

# Stage NE2000.COM. The dosbox-x-rig already has one — reuse it.
NE2000_COM="${NE2000_COM:-../dosbox-x-rig/NE2000.COM}"
if [ ! -f "$NE2000_COM" ]; then
    echo "NE2000.COM not found at $NE2000_COM — fetch via" >&2
    echo "  ../dosbox-x-rig/fetch.sh" >&2
    exit 1
fi

# Self-signed cert for the host tls_server.py.
if [ ! -f test-server.crt ] || [ ! -f test-server.key ]; then
    openssl req -x509 -nodes -newkey rsa:2048 \
        -subj "/CN=tls-smoke-crynwr" \
        -addext "subjectAltName=IP:10.0.2.2,IP:127.0.0.1" \
        -days 30 \
        -keyout test-server.key -out test-server.crt 2>/dev/null
fi

FREEDOS_IMG=/tmp/freedos.img
TEST_IMG="$(pwd)/freedos-tls-crynwr.img"
LOG="$(pwd)/qemu-tls-crynwr.log"

if [ ! -f "$FREEDOS_IMG" ]; then
    curl -fsSL -o "$FREEDOS_IMG" \
        https://raw.githubusercontent.com/codercowboy/freedosbootdisks/master/bootdisks/freedos.boot.disk.1.4MB.img
fi

echo "[rig] building FAT12 image (with NE2000.COM) ..."
cp "$FREEDOS_IMG" "$TEST_IMG"
mcopy -i "$TEST_IMG" -o "$NE2000_COM" ::NE2000.COM
mcopy -i "$TEST_IMG" -o "$TLS_EXE" ::TLS.EXE

AUTOEXEC=$(mktemp)
{
    printf '@echo off\r\n'
    printf 'CTTY COM1\r\n'
    printf 'echo === tls_smoke crynwr rig ===\r\n'
    # Load Crynwr NE2000 packet driver at INT 0x60. Args:
    # software interrupt | IRQ | IO base.
    printf 'NE2000 0x60 9 0x300\r\n'
    printf 'echo --- pktdrv loaded ---\r\n'
    printf 'TLS.EXE\r\n'
    printf 'echo === rig done ===\r\n'
} > "$AUTOEXEC"
mcopy -i "$TEST_IMG" -o "$AUTOEXEC" ::AUTOEXEC.BAT
rm -f "$AUTOEXEC"

# Start host TLS echo server.
TLS_SERVER="$(cd ../tls-rig && pwd)/tls_server.py"
echo "[rig] starting tls_server.py on port $TLS_PORT ..."
python3 "$TLS_SERVER" --port "$TLS_PORT" --max-seconds 600 \
    --cert "$(pwd)/test-server.crt" --key "$(pwd)/test-server.key" \
    > tls-server.log 2>&1 &
SERVER_PID=$!

cleanup() {
    rc=$?
    [ -n "${QEMU_PID:-}" ] && kill -KILL "$QEMU_PID" 2>/dev/null || true
    [ -n "${SERVER_PID:-}" ] && kill -KILL "$SERVER_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
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
    -object filter-dump,id=f0,netdev=net0,file="$(pwd)/qemu-tls-crynwr.pcap" \
    -no-reboot \
    > "$LOG" 2>&1 &
QEMU_PID=$!

# Cap at 3 minutes. We don't need the full handshake to complete to
# answer the question — just need to see if pktdrv_init succeeds via
# Crynwr and lwIP delivers an ARP reply.
for _ in $(seq 1 180); do
    sleep 1
    if grep -q "smoke:PASS\|smoke:.*FAIL\|rig done\|s3:tcp-connected\|s3:closed\|pi:no-driver\|pi:access-fail" "$LOG" 2>/dev/null; then
        sleep 2
    fi
    if grep -q "rig done\|smoke:PASS\|smoke:.*FAIL" "$LOG" 2>/dev/null; then
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
echo "=== tls_server.log ==="
cat tls-server.log
echo
# Success criterion: pktdrv path got at least to access-ok and
# lwIP saw the link up. Full handshake would be a bonus but isn't
# the question this rig is asking.
if grep -q "s3:tcp-connected\|smoke:PASS" "$LOG" 2>/dev/null; then
    echo "PASS (Crynwr path delivered RX up to TCP connect)"
    exit 0
fi
echo "FAIL"
exit 1
