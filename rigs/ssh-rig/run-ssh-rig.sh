#!/usr/bin/env bash
# uc386 MicroPython end-to-end SSH rig.
#
# Boots FreeDOS in QEMU with NE2000 + SLIRP user-mode networking,
# runs MP.EXE with a tiny SSHTEST.PY that connects to a paramiko-
# based test server on the host (reachable as 10.0.2.2 via SLIRP),
# completes the SSH handshake (Curve25519 KEX + Ed25519 host key
# verify via TweetNaCl), authenticates as testuser/testpass, execs
# `echo SSH_RIG_OK`, and PASSes if it reads back the marker.
#
# Wire-level analogue of the TLS rig — proves the libssh2 + axtls
# + TweetNaCl stack can complete a real SSH session against a real
# server, not just expose API symbols.
#
# Prereqs:
#   - qemu-system-i386
#   - mtools
#   - python3 with paramiko (4.x)
#   - ssh-keygen
#   - MP.EXE built via addons/harness/exe.py (see ../README.md)
#
# Usage:
#   ./run-ssh-rig.sh                    # uses ../dosbox-x-rig/MP.EXE
#   ./run-ssh-rig.sh /path/to/MP.EXE
#
# Exits 0 on PASS, 1 on any other outcome.

set -eu

cd "$(dirname "$0")"

MP_EXE="${1:-../dosbox-x-rig/MP.EXE}"
NE2000_COM="../dosbox-x-rig/NE2000.COM"
SSH_PORT="${SSH_PORT:-2222}"

if [ ! -f "$MP_EXE" ]; then
    echo "MP.EXE not found at $MP_EXE — build via addons/harness/exe.py" >&2
    exit 1
fi
if [ ! -f "$NE2000_COM" ]; then
    echo "NE2000.COM not found at $NE2000_COM — needed for guest packet driver" >&2
    exit 1
fi

# 1. Generate a fresh Ed25519 host key if we don't have one yet.
#    Pinned across runs so the client's host-key fingerprint stays
#    stable. The TLS rig regenerates the cert each run because cert
#    validity windows matter; SSH host-key identity is just a trust
#    anchor and we're not pinning it on the client yet.
HOST_KEY="$(pwd)/ssh_host_ed25519"
if [ ! -f "$HOST_KEY" ]; then
    echo "[rig] generating Ed25519 host key ..."
    ssh-keygen -t ed25519 -N "" -f "$HOST_KEY" -q
fi

# 2. Build the FAT12 floppy: FreeDOS minimal + MP.EXE + NE2000.COM
#    + AUTOEXEC.BAT + SSHTEST.PY.
FREEDOS_IMG=/tmp/freedos.img
TEST_IMG="$(pwd)/freedos-ssh.img"
LOG="$(pwd)/qemu-ssh.log"

if [ ! -f "$FREEDOS_IMG" ]; then
    echo "[rig] fetching FreeDOS boot floppy ..."
    curl -fsSL -o "$FREEDOS_IMG" \
        https://raw.githubusercontent.com/codercowboy/freedosbootdisks/master/bootdisks/freedos.boot.disk.1.4MB.img
fi

echo "[rig] building FAT12 image ..."
cp "$FREEDOS_IMG" "$TEST_IMG"
mcopy -i "$TEST_IMG" -o "$MP_EXE"      ::MP.EXE
mcopy -i "$TEST_IMG" -o "$NE2000_COM"  ::NE2000.COM
# Paste-mode wrap so MP REPL ingests the script in one block,
# matching the TLS rig's COM1 paste-mode pattern.
SSHTEST_WRAPPED="$(pwd)/sshtest-wrapped.in"
{
    printf '\x05'
    cat ./SSHTEST.PY
    printf '\x04'
    printf '\n\x04'
} > "$SSHTEST_WRAPPED"
mcopy -i "$TEST_IMG" -o ./SSHTEST.PY ::SSHTEST.PY

AUTOEXEC=$(mktemp)
{
    printf '@echo off\r\n'
    printf 'CTTY COM1\r\n'
    printf 'echo === ssh rig (PM-native NE2000) ===\r\n'
    # PM-native NE2000 path: MP talks to the NIC directly via the
    # PM IO instructions in port/pktdrv_uc386dos.c's ne2k_*_direct
    # path. Bypasses Crynwr + DPMI 0x0301 + the rmstub bounce-buffer
    # race that drops back-to-back packets. NE2000.COM intentionally
    # not loaded so PM-native owns the card.
    printf 'MP.EXE\r\n'
    printf 'echo === rig done ===\r\n'
} > "$AUTOEXEC"
mcopy -i "$TEST_IMG" -o "$AUTOEXEC" ::AUTOEXEC.BAT
rm -f "$AUTOEXEC"

# 3. Start the paramiko-based SSH server in the background. Reaped
#    at exit. max-seconds covers boot + DHCP-static + handshake +
#    auth + exec with margin.
echo "[rig] starting ssh_server.py on port $SSH_PORT ..."
python3 ./ssh_server.py --port "$SSH_PORT" --max-seconds 700 \
    > ssh-server.log 2>&1 &
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

# 4. Boot QEMU.
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
    -object filter-dump,id=f0,netdev=net0,file=/tmp/qemu-ssh.pcap \
    -no-reboot \
    < "$SSHTEST_WRAPPED" > "$LOG" 2>&1 &
QEMU_PID=$!

# Cap at 240s.  Anchor PASS/FAIL to column 0 (with optional CR for
# DOS line endings) — paste-mode echoes the source back to the
# serial line as MP ingests it, so unanchored matches false-fire
# on the indented FAIL prints inside `if/else` blocks.
for _ in $(seq 1 600); do
    sleep 1
    if grep -qE $'(^SSHTEST: (PASS|FAIL)\r?$|rig done)' "$LOG" 2>/dev/null; then
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
echo "=== Captured COM1 (qemu-ssh.log) ==="
cat "$LOG"
echo
echo "=== ssh-server.log ==="
cat ssh-server.log

echo
echo "=== Result ==="
if grep -qE $'^SSHTEST: PASS\r?$' "$LOG" 2>/dev/null; then
    echo "PASS: end-to-end SSH handshake + auth + exec round-tripped the marker."
    exit 0
fi

echo "Progress markers seen in MP stdout:"
for marker in "SSHTEST: start" \
              "SSHTEST: ifup" \
              "SSHTEST: tcp_connected" \
              "SSHTEST: handshake_ok" \
              "SSHTEST: auth_ok" \
              "SSHTEST: exec_ok" \
              "SSHTEST: connect_failed" \
              "SSHTEST: session_failed" \
              "SSHTEST: auth_failed" \
              "SSHTEST: exec_failed"; do
    if grep -qE "^${marker}" "$LOG" 2>/dev/null; then
        echo "  YES: ${marker}"
    fi
done

echo "Host SSH server progress:"
grep -E "listening|connection from|handshake ok|exec command|sent marker" ssh-server.log \
    2>/dev/null | sed 's/^/  /'

echo "FAIL: SSHTEST: PASS marker missing; see logs above"
exit 1
