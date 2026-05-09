#!/usr/bin/env bash
# Build PMWBUG.EXE — the minimal DOSBox-X PMODE/W INT 21h bug
# reproducer. Standalone: no uc386, no MicroPython, no exe.py
# bridge — just nasm + wlink + the bundled PMODE/W stub.
#
# Requires:
#   - nasm
#   - Open Watcom V2 (provides wlink + binw/pmodew.exe). Set the
#     WATCOM environment variable to the install dir, or have
#     `wlink` on PATH.
#
# Output: ./PMWBUG.EXE (PMODE/W-bound LE binary, ~770 KB —
# almost all of which is the high-VA padding).

set -eu

cd "$(dirname "$0")"

if ! command -v nasm >/dev/null; then
    echo "build.sh: nasm not found" >&2
    exit 1
fi

# Locate wlink. Open Watcom V2 ships it under $WATCOM/binl64/wlink
# (Linux 64-bit) or $WATCOM/binl/wlink (Linux 32-bit).
WLINK=""
for c in \
    "${WATCOM:-}/binl64/wlink" \
    "${WATCOM:-}/binl/wlink" \
    "$HOME/.local/opt/watcom/binl64/wlink" \
    "$HOME/.local/opt/watcom/binl/wlink" \
    "$(command -v wlink || true)" \
; do
    if [ -n "$c" ] && [ -x "$c" ]; then
        WLINK="$c"
        break
    fi
done
if [ -z "$WLINK" ]; then
    echo "build.sh: wlink not found — install Open Watcom V2" >&2
    echo "  https://github.com/open-watcom/open-watcom-v2/releases" >&2
    exit 1
fi

# wlink needs WATCOM in env to find its stub library.
if [ -z "${WATCOM:-}" ]; then
    WATCOM="$(dirname "$(dirname "$WLINK")")"
    export WATCOM
fi

# Locate the PMODE/W stub binary so wlink can BIND it as the
# MZ portion of the .exe (without `option stub=...`, wlink
# emits a 371-byte stub-only .exe whose MZ stub just prints
# "This is a PMODE/W executable" and exits).
PMODEW_STUB=""
for c in "$WATCOM/binw/pmodew.exe" "$WATCOM/binnt/pmodew.exe"; do
    if [ -f "$c" ]; then
        PMODEW_STUB="$c"
        break
    fi
done
if [ -z "$PMODEW_STUB" ]; then
    echo "build.sh: pmodew.exe stub not found under \$WATCOM/binw or \$WATCOM/binnt" >&2
    echo "  WATCOM=$WATCOM" >&2
    exit 1
fi

echo "build.sh: nasm  -> $(command -v nasm)"
echo "build.sh: wlink -> $WLINK"
echo "build.sh: stub  -> $PMODEW_STUB"

nasm -f obj -o pmwbug.obj pmwbug.asm

"$WLINK" \
    system pmodew \
    name PMWBUG.EXE \
    file pmwbug.obj \
    option stack=64k \
    option start=_start \
    option stub="$PMODEW_STUB"

ls -la PMWBUG.EXE
