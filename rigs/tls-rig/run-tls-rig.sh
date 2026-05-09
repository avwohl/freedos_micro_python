#!/usr/bin/env bash
# uc386 MicroPython end-to-end TLS rig.
#
# Boots FreeDOS in QEMU with NE2000 + SLIRP user-mode networking,
# runs MP.EXE under PMODE/W with a tiny test script (TLSTEST.PY)
# that initialises lwIP, gets a DHCP lease, opens a TCP socket to
# the host (reachable as 10.0.2.2 via SLIRP), and does a real TLS
# handshake against tls_server.py running on the host. The server
# responds with a fixed marker, the script prints PASS, the rig
# extracts the floppy's TLSOUT.TXT and greps for the marker.
#
# This is the wire-level analogue of the dos_emu library smoke
# tests (`test_micropython_ssl_*`). The library tests prove the
# axtls integration parses certs and sets up SSLContext; this
# rig proves it can actually finish a handshake against a peer.
#
# Prereqs:
#   - qemu-system-i386 (brew install qemu / apt install qemu-system-x86)
#   - mtools (brew install mtools / apt install mtools)
#   - openssl (system)
#   - python3 (with stdlib ssl)
#   - MP.EXE built via addons/harness/exe.py (see ../README.md)
#
# Usage:
#   ./run-tls-rig.sh                    # uses ../dosbox-x-rig/MP.EXE
#   ./run-tls-rig.sh /path/to/MP.EXE
#
# Output: prints the captured TLSOUT.TXT and exits 0 on PASS, 1 on
# anything else.

set -eu

cd "$(dirname "$0")"

MP_EXE="${1:-../dosbox-x-rig/MP.EXE}"
NE2000_COM="../dosbox-x-rig/NE2000.COM"
TLS_PORT="${TLS_PORT:-8443}"

if [ ! -f "$MP_EXE" ]; then
    echo "MP.EXE not found at $MP_EXE — build via addons/harness/exe.py" >&2
    exit 1
fi
if [ ! -f "$NE2000_COM" ]; then
    echo "NE2000.COM not found at $NE2000_COM — needed for guest packet driver" >&2
    exit 1
fi

# 1. Generate a fresh self-signed cert (CN=10.0.2.2) every run. The
#    guest will load this exact PEM via load_verify_locations and
#    verify_mode=CERT_REQUIRED, so the handshake exercises the full
#    chain-validation path against a known-good peer.
echo "[rig] generating test cert ..."
openssl req -x509 -nodes -newkey rsa:2048 \
    -subj "/CN=uc386-tls-rig" \
    -addext "subjectAltName=IP:10.0.2.2,IP:127.0.0.1" \
    -days 30 \
    -keyout test-server.key -out test-server.crt 2>/dev/null

# Single PEM with just the cert — what the guest needs as a CA bundle.
cp test-server.crt test-server-ca.pem

# 2. Build the FAT12 floppy: FreeDOS minimal + MP.EXE + NE2000.COM
#    + AUTOEXEC.BAT + TLSTEST.PY + TESTCA.PEM (the cert).
FREEDOS_IMG=/tmp/freedos.img
TEST_IMG="$(pwd)/freedos-tls.img"
LOG="$(pwd)/qemu-tls.log"

if [ ! -f "$FREEDOS_IMG" ]; then
    echo "[rig] fetching FreeDOS boot floppy ..."
    curl -fsSL -o "$FREEDOS_IMG" \
        https://raw.githubusercontent.com/codercowboy/freedosbootdisks/master/bootdisks/freedos.boot.disk.1.4MB.img
fi

echo "[rig] building FAT12 image ..."
cp "$FREEDOS_IMG" "$TEST_IMG"
mcopy -i "$TEST_IMG" -o "$MP_EXE"      ::MP.EXE
mcopy -i "$TEST_IMG" -o "$NE2000_COM"  ::NE2000.COM
# Wrap TLSTEST.PY in MP REPL paste-mode markers so the whole script
# enters as one block instead of being re-parsed line-by-line through
# the slow COM1 INT 21h AH=01 character path. Ctrl-E (0x05) opens
# paste mode; Ctrl-D (0x04) closes it and triggers execution. A
# trailing newline + Ctrl-D exits the REPL after the script returns.
#
# We feed this stream into QEMU's -serial stdio rather than redirecting
# MP's stdin from a file on the floppy. The DOS-side `<` redirection
# under PMODE/W deterministically aborts at MP's first read with
# "Error reading from drive A: unknown command given to driver" once
# the binary reaches a certain size (verified: TYPE TLSTEST.PY in the
# same AUTOEXEC works fine, only MP's INT 21h AH=3F on a redirected
# handle blows up). Going through CTTY COM1's console path is a
# completely different DOS code path and sidesteps the bug.
TLSTEST_WRAPPED="$(pwd)/tlstest-wrapped.in"
{
    printf '\x05'
    cat ./TLSTEST.PY
    printf '\x04'
    printf '\n\x04'
} > "$TLSTEST_WRAPPED"
# Still copy TLSTEST.PY onto the floppy so any future probe / fallback
# can read it; not required for the live REPL feed.
mcopy -i "$TEST_IMG" -o ./TLSTEST.PY ::TLSTEST.PY
mcopy -i "$TEST_IMG" -o ./test-server-ca.pem ::TESTCA.PEM

# AUTOEXEC.BAT: load the packet driver at INT 0x60 / IRQ 9 / IO 0x300
# (matches QEMU's ne2k_isa default + pktdrv_uc386dos.c's IVT scan
# target), then run MP.EXE with the test script as stdin and stdout
# captured in TLSOUT.TXT. AFTER MP exits the 'DONE' marker prints to
# COM1 (no CTTY — DOS console is screen) so the rig can detect
# completion via the serial log.
AUTOEXEC=$(mktemp)
# CTTY COM1 routes DOS console I/O to the serial port so we see MP's
# output live in qemu-tls.log. Without CTTY, file-redirected output
# from MP.EXE stays buffered and doesn't make it back to the host
# until QEMU exits. mp-rig.yml uses the same CTTY pattern and that's
# the one that shows "MicroPython" banner cleanly.
{
    printf '@echo off\r\n'
    printf 'CTTY COM1\r\n'
    printf 'echo === before NE2000 ===\r\n'
    printf 'NE2000 0x60 9 0x300\r\n'
    printf 'echo === after NE2000 ===\r\n'
    # MP.EXE without `<` redirection: stdin = console = COM1 (via
    # CTTY). The host feeds the paste-mode-wrapped script through
    # QEMU's -serial stdio (see TLSTEST_WRAPPED below).
    printf 'MP.EXE\r\n'
    printf 'echo === rig done ===\r\n'
} > "$AUTOEXEC"
mcopy -i "$TEST_IMG" -o "$AUTOEXEC" ::AUTOEXEC.BAT
rm -f "$AUTOEXEC"

# 3. Start the host TLS server in the background. Reaped at exit.
# max-seconds covers the full QEMU lifetime (boot + DHCP + TLS) plus
# margin so the server is still listening when MP's lwIP finally
# routes a packet at it.
echo "[rig] starting tls_server.py on port $TLS_PORT ..."
python3 ./tls_server.py --port "$TLS_PORT" --max-seconds 300 \
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

# 4. Boot QEMU with NE2000 (ne2k_isa, the Crynwr driver target) on
#    its conventional IO 0x300 / IRQ 9, plugged into a SLIRP user-mode
#    netdev. SLIRP's built-in DHCP server hands the guest 10.0.2.15
#    with gateway 10.0.2.2 — that's the host's view of itself, where
#    the TLS server is listening. No hostfwd needed; the guest reaches
#    the server by talking to the gateway.
echo "[rig] booting QEMU + FreeDOS + NE2000 (log: $LOG) ..."
# Feed the paste-mode-wrapped TLSTEST.PY into QEMU's serial stdio so
# COM1 RX delivers it to MP.EXE's console-read path. QEMU buffers
# bytes in the chardev → 8250 RX FIFO until the guest drains them,
# so timing is forgiving (bytes sit in the buffer through DOS boot
# + AUTOEXEC + NE2000 load until MP starts reading).
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
    < "$TLSTEST_WRAPPED" > "$LOG" 2>&1 &
QEMU_PID=$!

# Watch for the success marker (or rig-done from autoexec) in
# COM1 output. With CTTY COM1, MP's stdout flows live to the
# serial line and ends up in $LOG. Cap at 240s to give DHCP +
# TLS handshake plenty of room.
for _ in $(seq 1 240); do
    sleep 1
    if grep -q "TLSTEST: PASS\|TLSTEST: FAIL\|rig done" "$LOG" 2>/dev/null; then
        # Give a beat for any trailing output to flush.
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
echo "=== Captured COM1 (qemu-tls.log) ==="
cat "$LOG"
echo
echo "=== tls-server.log (host TLS server) ==="
cat tls-server.log

echo
echo "=== Result ==="
# Anchor checks to actual print output (column 0), not the
# source-code echoes paste-mode mirrors back to the serial line
# as it ingests the script — those lines are indented inside
# `if/for/with` blocks and would false-positive any non-anchored
# grep.
if grep -qE '^TLSTEST: PASS$' "$LOG" 2>/dev/null; then
    echo "PASS: end-to-end TLS handshake completed; uc386 MP read decrypted bytes from the host TLS server."
    exit 0
fi

echo "Progress markers seen in MP stdout:"
for marker in "TLSTEST: start" \
              "TLSTEST: pre_lwip_reset" \
              "TLSTEST: post_lwip_reset" \
              "TLSTEST: pre_eth_init" \
              "TLSTEST: post_eth_init" \
              "TLSTEST: ifup" \
              "TLSTEST: tcp_connected" \
              "TLSTEST: ctx_ready" \
              "TLSTEST: handshake_ok" \
              "TLSTEST: received" \
              "TLSTEST: dhcp_failed" \
              "TLSTEST: connect_failed" \
              "TLSTEST: handshake_failed"; do
    if grep -qE "^${marker}" "$LOG" 2>/dev/null; then
        echo "  YES: ${marker}"
    fi
done

echo "Host TLS server progress:"
grep -E "listening|connection from|handshake ok|sent " tls-server.log \
    2>/dev/null | sed 's/^/  /'

echo "FAIL: TLSTEST: PASS marker missing; see logs above"
exit 1
