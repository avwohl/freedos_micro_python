#!/usr/bin/env bash
# Variant of run-tls-rig.sh that runs READTEST.PY instead of TLSTEST.PY.
# Goal: prove (or refute) the hypothesis that NE2000 IRQ activity is
# what stalls subsequent INT 21h reads. With the chip's NE_IMR=0 and
# the slave PIC's IRQ-9 line masked (pktdrv_uc386dos.c:ne2k_init_direct),
# `f.read()` after `eth_init()` should return cleanly. If READTEST: PASS
# fires, the IRQ theory is correct and the same change will likely
# unhang wrap_socket too. If `pre_read` lands but `post_read` doesn't,
# the IRQ is not the culprit — look elsewhere (axtls RNG, lwIP wait
# loop, etc.).
#
# Reuses the network bring-up scaffolding from run-tls-rig.sh; no host
# TLS server needed.

set -eu

cd "$(dirname "$0")"

MP_EXE="${1:-../dosbox-x-rig/MP.EXE}"
NE2000_COM="../dosbox-x-rig/NE2000.COM"

if [ ! -f "$MP_EXE" ]; then
    echo "MP.EXE not found at $MP_EXE — build via freedos-micropython port" >&2
    exit 1
fi
if [ ! -f "$NE2000_COM" ]; then
    echo "NE2000.COM not found at $NE2000_COM" >&2
    exit 1
fi

FREEDOS_IMG=/tmp/freedos.img
TEST_IMG="$(pwd)/freedos-read.img"
LOG="$(pwd)/qemu-read.log"

if [ ! -f "$FREEDOS_IMG" ]; then
    echo "[rig] fetching FreeDOS boot floppy ..."
    curl -fsSL -o "$FREEDOS_IMG" \
        https://raw.githubusercontent.com/codercowboy/freedosbootdisks/master/bootdisks/freedos.boot.disk.1.4MB.img
fi

# Generate a small TESTCA.PEM file to read. Content doesn't matter —
# we only care whether the read returns. Use the existing self-signed
# cert if present, else a tiny placeholder.
if [ ! -f test-server-ca.pem ]; then
    echo "[rig] generating placeholder TESTCA.PEM ..."
    openssl req -x509 -nodes -newkey rsa:1024 \
        -subj "/CN=read-rig-placeholder" \
        -days 1 \
        -keyout /tmp/_read_rig.key -out /tmp/_read_rig.crt 2>/dev/null
    cp /tmp/_read_rig.crt test-server-ca.pem
    rm -f /tmp/_read_rig.key /tmp/_read_rig.crt
fi

echo "[rig] building FAT12 image ..."
cp "$FREEDOS_IMG" "$TEST_IMG"
mcopy -i "$TEST_IMG" -o "$MP_EXE"      ::MP.EXE
mcopy -i "$TEST_IMG" -o "$NE2000_COM"  ::NE2000.COM
mcopy -i "$TEST_IMG" -o ./READTEST.PY  ::READTEST.PY
mcopy -i "$TEST_IMG" -o ./test-server-ca.pem ::TESTCA.PEM

# Same paste-mode wrapping as run-tls-rig.sh — feed READTEST.PY into
# MP's REPL via COM1 stdio.
READTEST_WRAPPED="$(pwd)/readtest-wrapped.in"
{
    printf '\x05'
    cat ./READTEST.PY
    printf '\x04'
    printf '\n\x04'
} > "$READTEST_WRAPPED"

AUTOEXEC=$(mktemp)
{
    printf '@echo off\r\n'
    printf 'CTTY COM1\r\n'
    printf 'echo === read-rig (PM-native NE2000, no Crynwr) ===\r\n'
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
    -no-reboot \
    < "$READTEST_WRAPPED" > "$LOG" 2>&1 &
QEMU_PID=$!

# 90 s is plenty: net init + open + a 64-byte read should be well
# under 10 s. If we don't see PASS by then, the read is hung.
for _ in $(seq 1 90); do
    sleep 1
    if grep -q "READTEST: PASS\|READTEST: FAIL\|rig done" "$LOG" 2>/dev/null; then
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
echo "=== Captured COM1 (qemu-read.log) ==="
cat "$LOG"

echo
echo "=== Result ==="
if grep -qE '^READTEST: PASS$' "$LOG" 2>/dev/null; then
    echo "PASS: f.read() returned after eth_init — IRQ-mask hypothesis confirmed."
    exit 0
fi

echo "Progress markers seen:"
for marker in "READTEST: start" \
              "READTEST: post_lwip_reset" \
              "READTEST: post_eth_init" \
              "READTEST: post_set_static" \
              "ne2k:reset-ok" \
              "ne2k:mac-read" \
              "ne2k:pic-mask" \
              "ne2k:ready" \
              "READTEST: pre_open" \
              "READTEST: post_open" \
              "READTEST: pre_read" \
              "READTEST: post_read"; do
    if grep -q "${marker}" "$LOG" 2>/dev/null; then
        echo "  YES: ${marker}"
    fi
done

echo "FAIL: see logs above"
exit 1
