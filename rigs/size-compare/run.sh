#!/bin/sh
# size-compare rig — the MicroPython port is built only through
# uc386 (pure-Python C23 → NASM → PMODE/W flat binary). This rig
# answers the size question the sibling effort cares about with
# numbers instead of assertions: of the N source TUs this port
# feeds uc386, how many do the other DOS C toolchains accept, and
# how big is uc386's shipped artifact next to them.
#
# A *full* DJGPP or Open Watcom DOS link of MicroPython is its own
# port (a working `mpconfigport.h` + HAL + qstr wiring per
# toolchain). What IS meaningful — and is the methodology
# `NOTES.md` already uses for uc386 — is a per-TU compile triage
# over the active source list. MicroPython's core (`upstream/py/`)
# is written to be portable to bare-metal, so a foreign compiler
# gets meaningfully far; the triage quantifies exactly how far.
#
# Nothing here is needed to build or use the port. Both foreign
# toolchains are OPTIONAL — a missing one is reported and skipped.
#
# Usage (from a populated work dir — after `freedos-micropython
# fetch` and `freedos-micropython build`, so build/_port_sources.txt
# and the generated headers exist):
#
#     FREEDOS_MP_WORK=/path/to/work-dir sh rigs/size-compare/run.sh
#     # …or just run it from inside that work dir.
#
# Install the optional toolchains: see docs/COMPILER_COMPARISON.md.

set -eu

# --- locate the populated work dir ---------------------------------
WORK="${FREEDOS_MP_WORK:-$PWD}"
if [ ! -f "$WORK/build/_port_sources.txt" ]; then
    echo "size-compare: no build/_port_sources.txt under '$WORK'." >&2
    echo "  Run 'freedos-micropython fetch' then '… build' first," >&2
    echo "  or set FREEDOS_MP_WORK=<your work dir>." >&2
    exit 2
fi
cd "$WORK"

# --- find a python that can import uc386 (for lib/include + the
#     vendored watcom_dosbox driver) -------------------------------
UC_INC=""
UCPY="python3"
for PY in python3 python3.14 python3.12 \
          "$HOME/src/uc386/.venv/bin/python" ../uc386/.venv/bin/python; do
    command -v "$PY" >/dev/null 2>&1 || [ -x "$PY" ] || continue
    UC_INC="$("$PY" - <<'PY' 2>/dev/null || true
import importlib.util, pathlib
s = importlib.util.find_spec("uc386")
if s and s.origin:
    p = pathlib.Path(s.origin).parent / "lib" / "include"
    if p.is_dir():
        print(p)
PY
)"
    [ -n "$UC_INC" ] && { UCPY="$PY"; break; }
done
if [ -z "$UC_INC" ]; then
    for c in ../uc386/src/uc386/lib/include \
             "$HOME/src/uc386/src/uc386/lib/include"; do
        [ -d "$c" ] && { UC_INC="$(cd "$c" && pwd)"; break; }
    done
fi
[ -n "$UC_INC" ] || { echo "size-compare: can't locate uc386 lib/include" \
    "(install uc386 or keep a sibling ../uc386 checkout)." >&2; exit 2; }

# Include / define set — kept in lockstep with the uc386 invocation
# in src/freedos_micro_python/scripts/build_port.sh. If that list
# changes there, mirror it here.
INCS="-I $UC_INC -I upstream -I upstream/lib/lwip/src/include \
-I upstream/extmod/lwip-include -I upstream/lib/axtls/ssl \
-I upstream/lib/axtls/crypto -I upstream/extmod/axtls-include \
-I upstream/lib/libssh2/include -I upstream/lib/libssh2/src \
-I upstream/lib/tweetnacl -I uc386-dos -I build"
DEFS="-D__linux__=1 -DNDEBUG=1 -DMICROPY_SSL_AXTLS=1 -DMICROPY_PY_SSL=1 \
-DLIBSSH2_AXTLS=1 -DHAVE_SELECT=1 -DHAVE_SYS_SELECT_H=1 \
-DHAVE_SYS_UIO_H=1 -Dmp_stream_errno=errno"

SOURCES="$(grep -vE '^\s*#|^\s*$' build/_port_sources.txt)"
N_TOTAL="$(printf '%s\n' "$SOURCES" | grep -c . || true)"

echo "================================================================"
echo " MicroPython port — cross-compiler size / portability triage"
echo " frontier: $N_TOTAL source TUs (build/_port_sources.txt)"
echo "================================================================"

# --- uc386 baseline ------------------------------------------------
if [ -f build/micropython.bin ]; then
    UC_SZ="$(wc -c < build/micropython.bin | tr -d ' ')"
    echo "uc386   : build/micropython.bin = ${UC_SZ} bytes (shipped"
    echo "          PMODE/W flat .bin; boots the REPL under dos_emu)"
else
    echo "uc386   : build/micropython.bin absent (run 'freedos-"
    echo "          micropython port'; ~444 KB at EXTRA_FEATURES per"
    echo "          NOTES.md)"
fi

# --- DJGPP per-TU triage -------------------------------------------
DJGPP="${DJGPP:-$HOME/.local/opt/djgpp/bin/i586-pc-msdosdjgpp-gcc}"
command -v i586-pc-msdosdjgpp-gcc >/dev/null 2>&1 && \
    DJGPP="$(command -v i586-pc-msdosdjgpp-gcc)"
if [ -x "$DJGPP" ]; then
    ok=0; fail=0; obj_bytes=0
    tmp="$(mktemp -d)"; : > "$tmp/fail.log"
    for f in $SOURCES; do
        [ -f "$f" ] || continue
        o="$tmp/$(echo "$f" | tr '/.' '__').o"
        # shellcheck disable=SC2086
        if "$DJGPP" -c -w -O2 $DEFS $INCS "$f" -o "$o" 2>>"$tmp/fail.log"
        then
            ok=$((ok + 1))
            obj_bytes=$((obj_bytes + $(wc -c < "$o" | tr -d ' ')))
        else
            fail=$((fail + 1)); echo "  FAIL $f" >> "$tmp/fail.log"
        fi
    done
    echo "DJGPP   : ${ok}/${N_TOTAL} TUs compile (-O2); ${obj_bytes}"
    echo "          bytes of .o total. Failures → build/_djgpp_triage.log"
    cp "$tmp/fail.log" build/_djgpp_triage.log 2>/dev/null || true
    rm -rf "$tmp"
else
    echo "DJGPP   : skipped (no i586-pc-msdosdjgpp-gcc — see docs)"
fi

# --- Watcom (leaf-TU sample) via DOSBox-X --------------------------
WC_DIR="${WATCOM_DOS_DIR:-$HOME/.local/opt/watcom-dos}"
WCROOT="$("$UCPY" - <<'PY' 2>/dev/null || true
import importlib.util, pathlib
s = importlib.util.find_spec("uc386")
if s and s.origin:
    root = pathlib.Path(s.origin).parents[2]
    if (root / "addons" / "harness" / "watcom_dosbox.py").is_file():
        print(root)
PY
)"
if command -v dosbox-x >/dev/null 2>&1 && [ -f "$WC_DIR/binw/wcc386.exe" ] \
   && [ -n "$WCROOT" ]; then
    leaf=""
    for f in upstream/py/qstr.c upstream/py/vstr.c upstream/py/mpprint.c
    do [ -f "$f" ] && leaf="$leaf $f"; done
    if [ -n "$leaf" ]; then
        # shellcheck disable=SC2086
        if ( cd "$WCROOT" && WATCOM_DOS_DIR="$WC_DIR" "$UCPY" -m \
               addons.harness.watcom_dosbox $(for f in $leaf; do \
               echo "$WORK/$f"; done) -o "$WORK/build/_wc_leaf.exe" \
               $DEFS >/dev/null 2>&1 ); then
            echo "Watcom  : leaf py sample LINKED =" \
                 "$(wc -c < build/_wc_leaf.exe | tr -d ' ') bytes"
        else
            echo "Watcom  : leaf sample did not link (expected — needs"
            echo "          a Watcom mpconfigport/HAL; future work)."
        fi
    fi
else
    echo "Watcom  : skipped (need dosbox-x + \$WATCOM_DOS_DIR + a uc386"
    echo "          source checkout for watcom_dosbox — see docs)"
fi

echo "================================================================"
echo "uc386's number is a real, shippable DOS REPL. DJGPP/Watcom are"
echo "per-TU triage, NOT a linked interpreter — a foreign DOS port of"
echo "MicroPython is its own effort. See docs/COMPILER_COMPARISON.md."
