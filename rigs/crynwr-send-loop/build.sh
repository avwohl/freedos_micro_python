#!/usr/bin/env bash
# Build the crynwr-send-loop bench. Compiles main.c + the
# freedos_micro_python pktdrv_uc386dos.c with -DPKTDRV_FORCE_CRYNWR=1.
# No lwIP, no axtls, no TLS — just packet-driver init + send loop.
set -eu

cd "$(dirname "$0")"
HERE="$(pwd)"

FMP_WORKDIR="${FMP_WORKDIR:-/tmp/fmp_pypi_build}"
PORT="$(cd ../../src/freedos_micro_python/port && pwd)"
UC386_REPO="${UC386_REPO:-$(cd ../../../uc386 && pwd)}"
FMP_PY="${FMP_PY:-$(command -v python3.14 || command -v python3.12 || command -v python3)}"
DOS32A_EXE="${DOS32A_EXE:-$FMP_WORKDIR/dos32a.exe}"

if [ ! -f "$DOS32A_EXE" ]; then
    echo "crynwr_send_loop: DOS32A.EXE missing at $DOS32A_EXE." >&2
    exit 1
fi

UC386_LIB_INCLUDE=$("$FMP_PY" -c \
    "import uc386, pathlib; print(pathlib.Path(uc386.__file__).resolve().parent / 'lib' / 'include')")

mkdir -p build

# NO_IRQ_MASK=1 ./build.sh disables the DOS/32A IRQ-mask workaround
# in pktdrv_init so the bench reproduces the original GPF. Used to
# isolate whether a host swap (e.g. CWSDPMI) is what's keeping the
# send path alive, vs. our workaround.
EXTRA_DEFS=""
if [ "${NO_IRQ_MASK:-0}" = "1" ]; then
    EXTRA_DEFS="$EXTRA_DEFS -DPKTDRV_NO_IRQ_MASK_WORKAROUND=1"
fi

echo "crynwr_send_loop: compiling via uc386 ..."
"$FMP_PY" -m uc386.main \
    -I "$UC386_LIB_INCLUDE" \
    -I "$PORT" \
    -D__linux__=1 \
    -DNDEBUG=1 \
    -DPKTDRV_FORCE_CRYNWR=1 \
    $EXTRA_DEFS \
    "$PORT"/pktdrv_uc386dos.c \
    ./main.c \
    -o build/crynwr_send_loop.asm
echo "crynwr_send_loop: wrote build/crynwr_send_loop.asm ($(wc -c < build/crynwr_send_loop.asm | tr -d ' ') bytes)"

echo "crynwr_send_loop: linking via pyle + DOS/32A ..."
ASM_ABS="$HERE/build/crynwr_send_loop.asm"
OUT_ABS="$HERE/build/CRYN.EXE"
( cd "$UC386_REPO" && \
  "$FMP_PY" -m addons.harness.exe \
    "$ASM_ABS" \
    -o "$OUT_ABS" \
    --extender=dos32a \
    --stub-binary="$DOS32A_EXE" )
echo "crynwr_send_loop: wrote build/CRYN.EXE ($(wc -c < build/CRYN.EXE | tr -d ' ') bytes)"
