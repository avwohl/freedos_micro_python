#!/usr/bin/env bash
# Install the FreeDOS package with the real FreeDOS package installer,
# on a real FreeDOS kernel, and check that what lands on disk is what
# the package promised.
#
# This is the test that actually proves the packaging: not "does the
# zip look right on the host" but "does FDINST unpack it into the
# right places and does MP.EXE run from where it put it".
#
# FDINST is the small, network-free sibling of FDNPKG. Its docs say it
# "shares most of its source code with FDNPKG to ensure that both tools
# handle packages exactly the same way", so an FDINST install exercises
# the same unpack path a user's `FDNPKG install mpython` would.
#
# Layout: boot the 1.44 MB FreeDOS floppy as A:, attach a 32 MB
# partitioned FAT16 disk as C:, and let AUTOEXEC drive the install
# over COM1.
#
# Prereqs:
#   - qemu-system-i386  (brew install qemu)
#   - mtools            (brew install mtools)
#   - a built package   (python3 release/mkfdpkg.py --exe MP.EXE)
#
# Usage:
#   ./run-fdinst-rig.sh [path/to/mpython.zip]
#
# The captured COM1 log is written to qemu-fdinst.log next to this
# script.

set -eu

cd "$(dirname "$0")"

PKG="${1:-../../release/fdpkg/mpython.zip}"
if [ ! -f "$PKG" ]; then
    echo "[rig] package not found: $PKG" >&2
    echo "[rig] build it with: python3 release/mkfdpkg.py --exe /path/to/MP.EXE" >&2
    exit 1
fi

FREEDOS_IMG=/tmp/freedos.img
FDNPKG_ZIP=/tmp/fdnpkg.zip
HDD_IMG="$(pwd)/fdinst-hdd.img"
FLOPPY_IMG="$(pwd)/fdinst-boot.img"
LOG="$(pwd)/qemu-fdinst.log"

if [ ! -f "$FREEDOS_IMG" ]; then
    echo "[rig] fetching FreeDOS boot floppy ..."
    curl -fsSL -o "$FREEDOS_IMG" \
        https://raw.githubusercontent.com/codercowboy/freedosbootdisks/master/bootdisks/freedos.boot.disk.1.4MB.img
fi

# FDINST.EXE comes from the official fdnpkg package — we test against
# the real installer, not a reimplementation of it.
if [ ! -f "$FDNPKG_ZIP" ]; then
    echo "[rig] fetching fdnpkg package (for FDINST.EXE) ..."
    curl -fsSL --retry 3 -o "$FDNPKG_ZIP" \
        https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/latest/net/fdnpkg.zip
fi
rm -rf ./_fdnpkg && mkdir -p ./_fdnpkg
unzip -qo "$FDNPKG_ZIP" 'BIN/*' -d ./_fdnpkg

echo "[rig] building 32 MB FAT16 disk ..."
rm -f "$HDD_IMG"
dd if=/dev/zero of="$HDD_IMG" bs=1m count=32 2>/dev/null
MTOOLSRC="$(pwd)/_mtoolsrc"
printf 'drive c: file="%s" partition=1\n' "$HDD_IMG" > "$MTOOLSRC"
export MTOOLSRC
mpartition -I -c -s 63 -h 16 -t 65 c:
mformat c:

mcopy ./_fdnpkg/BIN/FDINST.EXE c:/FDINST.EXE
mcopy "$PKG"                   c:/MPYTHON.ZIP
mmd c:/FDOS
mmd c:/FDOS/BIN
mmd c:/FDOS/APPINFO      # FDINST writes the installed package's LSM here
mmd c:/FDOS/PACKAGES     # ... and its file list here
mmd c:/TEMP              # FDINST refuses to run without a writeable %TEMP%

# FDINST reads the same config as FDNPKG for its install directories.
# Keep it minimal: where devel packages go, where links go, and where
# sources go. `installsources 0` matches the FreeDOS default.
FDCFG=$(mktemp)
{
    printf 'maxcachetime 7200\r\n'
    printf 'installsources 0\r\n'
    printf 'skiplinks 0\r\n'
    printf 'dir devel c:\\devel\r\n'
    printf 'dir source c:\\fdos\\source\r\n'
    printf 'dir links c:\\fdos\\links\r\n'
} > "$FDCFG"
mcopy "$FDCFG" c:/FDNPKG.CFG
rm -f "$FDCFG"

echo "[rig] building boot floppy ..."
cp "$FREEDOS_IMG" "$FLOPPY_IMG"
AUTOEXEC=$(mktemp)
{
    printf '@echo off\r\n'
    printf 'CTTY COM1\r\n'
    printf 'SET DOSDIR=C:\\FDOS\r\n'
    printf 'SET FDNPKG.CFG=C:\\FDNPKG.CFG\r\n'
    printf 'SET TEMP=C:\\TEMP\r\n'
    printf 'C:\r\n'
    printf 'echo === FDINST install ===\r\n'
    printf 'FDINST install MPYTHON.ZIP\r\n'
    printf 'echo === installed tree ===\r\n'
    printf 'dir C:\\DEVEL\\MPYTHON\r\n'
    printf 'dir C:\\FDOS\\LINKS\r\n'
    printf 'dir C:\\FDOS\\APPINFO\r\n'
    printf 'echo === run it ===\r\n'
    printf 'C:\\DEVEL\\MPYTHON\\MP.EXE HELLO.PY\r\n'
    printf 'echo === rig done ===\r\n'
} > "$AUTOEXEC"
mcopy -i "$FLOPPY_IMG" -o "$AUTOEXEC" ::AUTOEXEC.BAT
rm -f "$AUTOEXEC"

# A script that proves the interpreter really ran, not just loaded.
HELLO=$(mktemp)
{
    printf 'import sys\r\n'
    printf 'print("FDINST-RIG:", 6*7, sys.implementation.name)\r\n'
} > "$HELLO"
mcopy "$HELLO" c:/HELLO.PY
rm -f "$HELLO"

cleanup() {
    rc=$?
    [ -n "${QEMU_PID:-}" ] && kill -KILL "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    return $rc
}
trap cleanup EXIT INT TERM

echo "[rig] booting QEMU (floppy A:, 32 MB disk C:) ..."
qemu-system-i386 \
    -display none \
    -serial stdio \
    -drive file="$FLOPPY_IMG",format=raw,if=floppy,index=0 \
    -drive file="$HDD_IMG",format=raw,if=ide,index=0,media=disk \
    -boot a \
    -m 32 \
    -cpu pentium \
    -no-reboot \
    < /dev/null > "$LOG" 2>&1 &
QEMU_PID=$!

for _ in $(seq 1 90); do
    sleep 1
    if grep -q "rig done\|Boot failed\|No bootable\|invalid system disk" "$LOG" 2>/dev/null; then
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
echo "=== Captured COM1 ($LOG) ==="
cat "$LOG"

echo
echo "=== Host-side check of the installed disk ==="
mdir -/ c:/ 2>/dev/null || true

echo
echo "=== Result ==="
fail=0

# Check the disk, not the console. Console text is easy to match by
# accident — an error message naming MP.EXE looks just like a
# successful directory listing to grep.
on_disk() {
    if mdir "$1" 2>/dev/null | grep -qi "$2"; then
        echo "  PASS  $3"
    else
        echo "  FAIL  $3"
        fail=1
    fi
}
in_log() {
    if grep -q "$1" "$LOG" 2>/dev/null; then
        echo "  PASS  $2"
    else
        echo "  FAIL  $2"
        fail=1
    fi
}

on_disk c:/DEVEL/MPYTHON  "MP *EXE"      "MP.EXE unpacked to C:\\DEVEL\\MPYTHON"
on_disk c:/DEVEL/MPYTHON  "README *TXT"  "docs unpacked alongside it"
on_disk c:/DEVEL/MPYTHON  "WGET *PY"     "bundled programs unpacked"
# Note the extension change: the LINKS/*.BAT we ship is only a marker
# holding the target path. FDINST replaces it with a real ~80 byte
# .COM launcher, so what lands on disk is MP.COM, not MP.BAT.
on_disk c:/FDOS/LINKS     "MP *COM"      "MP link installed into the links dir"
on_disk c:/FDOS/APPINFO   "MPYTHON *LSM" "LSM registered in %DOSDIR%\\APPINFO"
in_log  "FDINST-RIG: 42 micropython"     "interpreter runs from its installed path"

if [ "$fail" -eq 0 ]; then
    echo "FDINST-RIG: PASS"
else
    echo "FDINST-RIG: FAIL"
fi
exit "$fail"
