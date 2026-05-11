#!/usr/bin/env bash
# Run TLS.EXE under DOSBox-X dynamic_x86 core against a host
# tls_server.py. Mirrors run.sh (QEMU + FreeDOS floppy) but uses
# DOSBox-X's built-in DOS + NE2000 + SLIRP — no FreeDOS image
# needed, no extra boot time, and the dynamic core does x86 JIT
# instead of QEMU TCG's pessimal-on-ARM software translation.
set -eu

cd "$(dirname "$0")"

TLS_EXE="${1:-./build/TLS.EXE}"
TLS_PORT="${TLS_PORT:-8443}"

if ! command -v dosbox-x >/dev/null 2>&1; then
    echo "dosbox-x not on PATH. brew install dosbox-x" >&2
    exit 1
fi

if [ ! -f "$TLS_EXE" ]; then
    echo "TLS.EXE not found at $TLS_EXE — run ./build.sh first" >&2
    exit 1
fi

# Generate a self-signed cert if missing.
if [ ! -f test-server.crt ] || [ ! -f test-server.key ]; then
    openssl req -x509 -nodes -newkey rsa:2048 \
        -subj "/CN=tls-smoke-rig" \
        -addext "subjectAltName=IP:10.0.2.2,IP:127.0.0.1" \
        -days 30 \
        -keyout test-server.key -out test-server.crt 2>/dev/null
fi

# Stage TLS.EXE next to the config so the autoexec mount picks it up.
cp "$TLS_EXE" ./TLS.EXE
rm -f RIG.LOG

# Start the host TLS echo server. Reuse the one in rigs/tls-rig/.
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
    [ -n "${SERVER_PID:-}" ] && kill -KILL "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    return $rc
}
trap cleanup EXIT INT TERM

echo "[rig] launching DOSBox-X dynamic_x86 ..."
# -silent suppresses the SDL window; autoexec runs TLS.EXE and exits.
# Cap at 10 minutes — same budget as run.sh.
( timeout 600 dosbox-x -silent -conf dosbox-x.conf >/dev/null 2>&1 || true ) &
DOSBOX_PID=$!

# Stream the in-progress RIG.LOG so we can see progress (DOSBox-X
# flushes the DOS file write through to the mounted directory).
for _ in $(seq 1 600); do
    sleep 1
    if grep -q "smoke:PASS\|smoke:.*FAIL\|rig done" RIG.LOG 2>/dev/null; then
        sleep 2
        break
    fi
    if ! kill -0 "$DOSBOX_PID" 2>/dev/null; then
        break
    fi
done
kill -KILL "$DOSBOX_PID" 2>/dev/null || true
wait "$DOSBOX_PID" 2>/dev/null || true

echo
echo "=== RIG.LOG ==="
cat RIG.LOG 2>/dev/null || echo "(no RIG.LOG produced)"
echo
echo "=== tls-server.log ==="
cat tls-server.log
echo
if grep -q "smoke:PASS" RIG.LOG 2>/dev/null; then
    echo "PASS"
    exit 0
fi
echo "FAIL"
exit 1
