#!/bin/sh
# Fetch the third-party DOS binaries the rig needs.
#
# - NE2000.COM — Crynwr packet driver for the Novell NE2000 ISA NIC.
#   DOSBox-X's NE2000 emulation is a faithful clone of that hardware,
#   so this driver loads cleanly under it. Sourced from the Internet
#   Archive's "Ethernet Packet Drivers for MS-DOS" collection (the
#   community-curated mirror of the Crynwr 11.x distribution).
#
# Idempotent: re-running with the file already in place is a no-op.
set -eu
cd "$(dirname "$0")"

NE2000_URL="https://archive.org/download/lan-packet-drivers-for-ms-dos/drvlan/ne2000.zip"
NE2000_SHA1="$(printf '8693 NE2000.COM bytes')"  # informational only

if [ ! -f NE2000.COM ]; then
    echo "rig: fetching NE2000 packet driver from archive.org ..."
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    curl -fsSL "$NE2000_URL" -o "$tmp/ne2000.zip"
    unzip -q -o "$tmp/ne2000.zip" -d "$tmp"
    cp "$tmp/NE2000.COM" .
    echo "rig: NE2000.COM ($(wc -c < NE2000.COM) bytes) ready."
fi

# Sanity: print whether the binary that the rig runs on top is
# present yet. It's built by addons/harness/exe.py — Watcom-only,
# so on macOS this will usually be missing. The user copies one in
# from a Linux build.
if [ ! -f MP.EXE ] && [ ! -f mp.exe ]; then
    cat <<'WARN'
rig: MP.EXE not present.
     Build it on a Linux box:
         python -m addons.harness.exe \
             addons/gnu/micropython/build/micropython.bin \
             -o addons/gnu/micropython/dosbox-x-rig/MP.EXE
     Then re-run ./run.sh.
WARN
fi
