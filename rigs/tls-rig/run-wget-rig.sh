#!/usr/bin/env bash
# uc386 MicroPython end-to-end wget-over-TLS rig with cert
# verification.
#
# Same shape as run-tls-rig.sh, but instead of the raw TLS-echo
# server + TLSTEST.PY, we stand up the same tls_server.py in HTTP
# response mode and run WGETTEST.PY (which `import wget` and calls
# `wget.fetch(url, verify=True, ca_certs='TESTCA.PEM')`). PASS
# requires:
#   - lwIP TCP connect to host gateway succeeds,
#   - TLS handshake completes,
#   - axtls cert-chain verification accepts the rig's self-signed
#     CA (loaded via SSLContext.load_verify_locations),
#   - HTTP/1.0 GET + Connection: close exchange runs to EOF,
#   - the response body is streamed to WGETOUT.BIN on the floppy
#     and contains the rig-server's marker.
#
# Prereqs: qemu-system-i386, mtools, openssl, python3, plus
# MP.EXE built via addons/harness/exe.py (see ../README.md).
# WGET.PY is staged from ../../examples/wget.py at floppy build
# time so a single source-of-truth file ships both as the
# example and as the floppy module the rig imports.

set -eu

cd "$(dirname "$0")"

MP_EXE="${1:-../dosbox-x-rig/MP.EXE}"
NE2000_COM="../dosbox-x-rig/NE2000.COM"
WGET_SRC="../../examples/wget.py"
TLS_PORT="${TLS_PORT:-8443}"

for f in "$MP_EXE" "$NE2000_COM" "$WGET_SRC"; do
    if [ ! -f "$f" ]; then
        echo "missing: $f" >&2
        exit 1
    fi
done

# 1. Fresh self-signed cert + CA per run.
echo "[rig] generating test cert ..."
openssl req -x509 -nodes -newkey rsa:2048 \
    -subj "/CN=uc386-wget-rig" \
    -addext "subjectAltName=IP:10.0.2.2,IP:127.0.0.1" \
    -days 30 \
    -keyout test-server.key -out test-server.crt 2>/dev/null
cp test-server.crt test-server-ca.pem

# 2. Server body. The marker WGET_RIG_OK is what WGETTEST.PY
#    greps for. Pad to a known length so we exercise the
#    streaming read loop in wget.fetch (multiple buf_size chunks).
HTTP_BODY=/tmp/wget-rig-body.bin
{
    printf 'WGET_RIG_OK\n'
    # 256 deterministic bytes — `seq` would do but isn't portable.
    /Users/wohl/src/uc386/.venv/bin/python -c \
        'import sys; sys.stdout.buffer.write(bytes((i % 251) for i in range(256)))'
} > "$HTTP_BODY"

# 3. Build the FAT12 floppy: FreeDOS minimal + MP.EXE + NE2000.COM
#    + WGET.PY + WGETTEST.PY + TESTCA.PEM (the CA root) +
#    AUTOEXEC.BAT.
FREEDOS_IMG=/tmp/freedos.img
TEST_IMG="$(pwd)/freedos-wget.img"
LOG="$(pwd)/qemu-wget.log"

if [ ! -f "$FREEDOS_IMG" ]; then
    echo "[rig] fetching FreeDOS boot floppy ..."
    curl -fsSL -o "$FREEDOS_IMG" \
        https://raw.githubusercontent.com/codercowboy/freedosbootdisks/master/bootdisks/freedos.boot.disk.1.4MB.img
fi

# Stage WGET.PY onto the floppy. DOS uses 8.3 case-insensitive,
# so the import lookup `import wget` finds WGET.PY.
WGET_PY="$(pwd)/WGET.PY"
cp "$WGET_SRC" "$WGET_PY"

echo "[rig] building FAT12 image ..."
cp "$FREEDOS_IMG" "$TEST_IMG"
mcopy -i "$TEST_IMG" -o "$MP_EXE"      ::MP.EXE
mcopy -i "$TEST_IMG" -o "$NE2000_COM"  ::NE2000.COM
mcopy -i "$TEST_IMG" -o ./WGETTEST.PY  ::WGETTEST.PY
mcopy -i "$TEST_IMG" -o "$WGET_PY"     ::WGET.PY
mcopy -i "$TEST_IMG" -o ./test-server-ca.pem ::TESTCA.PEM

# Same paste-mode wrapper as the TLS rig — Ctrl-E enters, Ctrl-D
# fires execution, trailing Ctrl-D exits the REPL.
WGETTEST_WRAPPED="$(pwd)/wgettest-wrapped.in"
{
    printf '\x05'
    cat ./WGETTEST.PY
    printf '\x04'
    printf '\n\x04'
} > "$WGETTEST_WRAPPED"

# AUTOEXEC: load NE2000 packet driver + run MP.EXE with COM1
# console. After MP exits, mark the rig done so the host can
# detect completion without parsing the test-side output.
AUTOEXEC=$(mktemp)
{
    printf '@echo off\r\n'
    printf 'CTTY COM1\r\n'
    printf 'echo === wget rig (Crynwr NE2000.COM at INT 0x60) ===\r\n'
    printf 'NE2000 0x60 9 0x300\r\n'
    printf 'echo --- pktdrv loaded ---\r\n'
    printf 'MP.EXE\r\n'
    printf 'echo === rig done ===\r\n'
} > "$AUTOEXEC"
mcopy -i "$TEST_IMG" -o "$AUTOEXEC" ::AUTOEXEC.BAT
rm -f "$AUTOEXEC"

# 4. Start the host TLS server in HTTP mode.
echo "[rig] starting tls_server.py (HTTP mode) on port $TLS_PORT ..."
/Users/wohl/src/uc386/.venv/bin/python ./tls_server.py \
    --port "$TLS_PORT" \
    --max-seconds 700 \
    --http-body-file "$HTTP_BODY" \
    --http-path "/wget.bin" \
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

# 5. Boot QEMU + FreeDOS + NE2000 on the same SLIRP user-mode
# net the TLS rig uses (host gateway 10.0.2.2, guest 10.0.2.15).
echo "[rig] booting QEMU + FreeDOS + NE2000 (log: $LOG) ..."
qemu-system-i386 \
    -display none \
    -serial stdio \
    -fda "$TEST_IMG" \
    -boot a \
    -m 16 \
    -cpu pentium \
    -netdev user,id=net0 \
    -device ne2k_isa,netdev=net0,iobase=0x300,irq=9 \
    -object filter-dump,id=f0,netdev=net0,file=/tmp/qemu-wget.pcap \
    -no-reboot \
    < "$WGETTEST_WRAPPED" > "$LOG" 2>&1 &
QEMU_PID=$!

# 6. Watch for PASS/FAIL or `rig done` marker.
for _ in $(seq 1 600); do
    sleep 1
    if grep -qE $'(^WGETTEST: (PASS|FAIL)\r?$|rig done)' "$LOG" 2>/dev/null; then
        sleep 2
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        break
    fi
done
kill "$QEMU_PID" 2>/dev/null || true

echo
echo "=== Captured stdout (qemu-wget.log) ==="
cat "$LOG"

echo
echo "=== Result ==="
if tr -d '\r' < "$LOG" | grep -qE '^WGETTEST: PASS$' 2>/dev/null; then
    echo "PASS: wget --verify --ca-certs fetched the rig server's body."
    exit 0
fi

echo "FAIL: see qemu-wget.log + tls-server.log for diagnostics."
exit 1
