#!/usr/bin/env bash
# Build the bigint inner-loop bench. Trimmed copy of
# rigs/tls-smoke/build.sh — no lwIP, no axtls, no port glue.
# Just main.c + the uc386 libc bridge + DOS/32A.
set -eu

cd "$(dirname "$0")"
HERE="$(pwd)"

FMP_WORKDIR="${FMP_WORKDIR:-/tmp/fmp_pypi_build}"

UC386_REPO="${UC386_REPO:-$(cd ../../../uc386 && pwd)}"
FMP_PY="${FMP_PY:-$(command -v python3.14 || command -v python3.12 || command -v python3)}"
DOS32A_EXE="${DOS32A_EXE:-$FMP_WORKDIR/dos32a.exe}"

if [ ! -f "$DOS32A_EXE" ]; then
    echo "bigint_bench: DOS32A.EXE missing at $DOS32A_EXE." >&2
    exit 1
fi

UC386_LIB_INCLUDE=$("$FMP_PY" -c \
    "import uc386, pathlib; print(pathlib.Path(uc386.__file__).resolve().parent / 'lib' / 'include')")

mkdir -p build

echo "bigint_bench: compiling main.c via uc386 …"
"$FMP_PY" -m uc386.main \
    -I "$UC386_LIB_INCLUDE" \
    -D__linux__=1 \
    -DNDEBUG=1 \
    ./main.c \
    -o build/bigint_bench.asm
echo "bigint_bench: wrote build/bigint_bench.asm ($(wc -c < build/bigint_bench.asm | tr -d ' ') bytes)"

echo "bigint_bench: linking via pyle + DOS/32A …"
ASM_ABS="$HERE/build/bigint_bench.asm"
OUT_ABS="$HERE/build/BIGINT.EXE"
( cd "$UC386_REPO" && \
  "$FMP_PY" -m addons.harness.exe \
    "$ASM_ABS" \
    -o "$OUT_ABS" \
    --extender=dos32a \
    --stub-binary="$DOS32A_EXE" )
echo "bigint_bench: wrote build/BIGINT.EXE ($(wc -c < build/BIGINT.EXE | tr -d ' ') bytes)"
