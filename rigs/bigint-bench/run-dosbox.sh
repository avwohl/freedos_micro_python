#!/usr/bin/env bash
# Run BIGINT.EXE under DOSBox-X dynamic_x86 core. Same bench code
# the QEMU rig runs, but with a different emulator — the dynamic
# core should give a much better per-instruction cost model than
# QEMU TCG-on-ARM, so the codegen wins should land more visibly.
set -eu

cd "$(dirname "$0")"

if ! command -v dosbox-x >/dev/null 2>&1; then
    echo "dosbox-x not on PATH. brew install dosbox-x" >&2
    exit 1
fi

BIGINT_EXE="${1:-./build/BIGINT.EXE}"
if [ ! -f "$BIGINT_EXE" ]; then
    echo "BIGINT.EXE not found at $BIGINT_EXE — run ./build.sh first" >&2
    exit 1
fi

# DOSBox-X autoexec.bat reads from the mounted directory, so
# stage BIGINT.EXE alongside the conf.
cp "$BIGINT_EXE" ./BIGINT.EXE

rm -f RIG.LOG

dosbox-x -silent -conf dosbox-x.conf >/dev/null 2>&1 || true

echo
echo "=== RIG.LOG ==="
cat RIG.LOG 2>/dev/null || echo "(no RIG.LOG produced)"
echo

if grep -q "bench:done" RIG.LOG 2>/dev/null; then
    echo "PASS"
    exit 0
fi
echo "FAIL"
exit 1
