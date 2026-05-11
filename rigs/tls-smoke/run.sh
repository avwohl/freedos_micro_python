#!/usr/bin/env bash
# Run TLS.EXE under FreeDOS+QEMU with NE2000 + SLIRP. Mirrors
# freedos_micro_python/rigs/tls-rig/run-tls-rig.sh except it boots
# our standalone TLS.EXE instead of MP+TLSTEST.PY.

set -eu

cd "$(dirname "$0")"

TLS_EXE="${1:-./build/TLS.EXE}"
TLS_PORT="${TLS_PORT:-8443}"

if [ ! -f "$TLS_EXE" ]; then
    echo "TLS.EXE not found at $TLS_EXE — run ./build.sh first" >&2
    exit 1
fi

# Generate a self-signed cert that the test server will present.
if [ ! -f test-server.crt ] || [ ! -f test-server.key ]; then
    openssl req -x509 -nodes -newkey rsa:2048 \
        -subj "/CN=tls-smoke-rig" \
        -addext "subjectAltName=IP:10.0.2.2,IP:127.0.0.1" \
        -days 30 \
        -keyout test-server.key -out test-server.crt 2>/dev/null
fi

FREEDOS_IMG=/tmp/freedos.img
TEST_IMG="$(pwd)/freedos-tls.img"
LOG="$(pwd)/qemu-tls.log"

if [ ! -f "$FREEDOS_IMG" ]; then
    curl -fsSL -o "$FREEDOS_IMG" \
        https://raw.githubusercontent.com/codercowboy/freedosbootdisks/master/bootdisks/freedos.boot.disk.1.4MB.img
fi

echo "[rig] building FAT12 image ..."
cp "$FREEDOS_IMG" "$TEST_IMG"
mcopy -i "$TEST_IMG" -o "$TLS_EXE" ::TLS.EXE

AUTOEXEC=$(mktemp)
{
    printf '@echo off\r\n'
    printf 'CTTY COM1\r\n'
    printf 'echo === tls_smoke rig ===\r\n'
    printf 'TLS.EXE\r\n'
    printf 'echo === rig done ===\r\n'
} > "$AUTOEXEC"
mcopy -i "$TEST_IMG" -o "$AUTOEXEC" ::AUTOEXEC.BAT
rm -f "$AUTOEXEC"

# Start host TLS echo server. Reuse the one in rigs/tls-rig/.
TLS_SERVER="$(cd ../tls-rig && pwd)/tls_server.py"
if [ ! -f "$TLS_SERVER" ]; then
    echo "tls_server.py not found at $TLS_SERVER" >&2
    exit 1
fi
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
    -object filter-dump,id=f0,netdev=net0,file="$(pwd)/qemu-tls-smoke.pcap" \
    -no-reboot \
    > "$LOG" 2>&1 &
QEMU_PID=$!

for _ in $(seq 1 600); do
    sleep 1
    if grep -q "smoke:PASS\|smoke:.*FAIL\|rig done" "$LOG" 2>/dev/null; then
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
echo "=== tls_server.log ==="
cat tls-server.log
echo
if grep -q "smoke:PASS" "$LOG" 2>/dev/null; then
    echo "PASS"
    exit 0
fi
echo "FAIL"
exit 1
