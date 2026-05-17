#!/usr/bin/env bash
#
# Fetch the FreeDOS components we leaned on, plus the boot disk
# image the rigs use, and bundle them into a dated tarball.
#
# Idempotent: re-running with the same pins overwrites the existing
# tarball. Bump the pins below to roll forward.
#
# See release/README.md for the rationale (we pass FreeDOS sources
# along voluntarily, not because the license strictly requires it).
#
# Usage:
#   cd release/
#   ./fetch-freedos.sh
#
# Output:
#   release/freedos-sources-YYYY-MM-DD.tar.gz
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Pins. Bump and re-run when promoting to a new component release.
# ---------------------------------------------------------------------------

# FreeDOS kernel — github.com/FDOS/kernel
KERNEL_TAG="v2043"                 # 2043 stable, mid-2025
KERNEL_URL="https://github.com/FDOS/kernel/archive/refs/tags/${KERNEL_TAG}.tar.gz"

# FreeCOM (COMMAND.COM) — github.com/FDOS/freecom
FREECOM_TAG="v0.85a"               # stable since 2020-ish
FREECOM_URL="https://github.com/FDOS/freecom/archive/refs/tags/${FREECOM_TAG}.tar.gz"

# PMODE/W (Tran's DOS extender). Bundles source with the binary.
# Hosted at the FreeDOS file mirror.
PMODEW_VER="133"
PMODEW_URL="https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/devel/extender/pmodew/pmw${PMODEW_VER}.zip"

# The boot disk we test against (codercowboy's wrapper repo).
BOOT_URL="https://raw.githubusercontent.com/codercowboy/freedosbootdisks/master/bootdisks/freedos.boot.disk.1.4MB.img"

# ---------------------------------------------------------------------------
# Layout
# ---------------------------------------------------------------------------

HERE="$(cd "$(dirname "$0")" && pwd)"
DATESTAMP="$(date +%Y-%m-%d)"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

OUT_DIR="$HERE"
OUT="$OUT_DIR/freedos-sources-${DATESTAMP}.tar.gz"

ROOT="$STAGE/freedos-sources"
mkdir -p "$ROOT"/{kernel,freecom,pmodew,bootdisk}

# ---------------------------------------------------------------------------
# Fetch
# ---------------------------------------------------------------------------

echo "release: fetching FreeDOS kernel ${KERNEL_TAG} …"
curl -fsSL "$KERNEL_URL" -o "$STAGE/kernel.tar.gz"
tar -xzf "$STAGE/kernel.tar.gz" -C "$ROOT/kernel" --strip-components=1

echo "release: fetching FreeCOM ${FREECOM_TAG} …"
curl -fsSL "$FREECOM_URL" -o "$STAGE/freecom.tar.gz"
tar -xzf "$STAGE/freecom.tar.gz" -C "$ROOT/freecom" --strip-components=1

echo "release: fetching PMODE/W ${PMODEW_VER} …"
curl -fsSL "$PMODEW_URL" -o "$ROOT/pmodew/pmw${PMODEW_VER}.zip"

echo "release: fetching boot disk image …"
curl -fsSL "$BOOT_URL" -o "$ROOT/bootdisk/freedos.boot.1.4MB.img"

# ---------------------------------------------------------------------------
# Compose top-level README + license summary
# ---------------------------------------------------------------------------

cat > "$ROOT/README.md" <<'EOF'
# FreeDOS source archive

This tarball ships the FreeDOS components that the
`freedos_micro_python` project leaned on during development.

It is distributed in the spirit of paying a debt to FreeDOS, not
because the license strictly requires it: `freedos_micro_python`
uses FreeDOS as a runtime target (boots it under QEMU and runs
MP.EXE on top), not as a base we modify.

Layout:

  kernel/              FDOS/kernel source — the FreeDOS kernel.
  freecom/             FDOS/freecom source — COMMAND.COM.
  pmodew/              PMODE/W (Tran's DOS extender) zip with sources.
  bootdisk/            The 1.4 MB boot floppy image we test against.

Each component carries its own license file inside its directory.
See LICENSE-summary.md at the top of this tarball for a one-screen
overview.
EOF

cat > "$ROOT/LICENSE-summary.md" <<EOF
# License summary

  - **kernel/** — FreeDOS kernel, GPL-2.0. Authors: Pat Villani,
    Tom Ehlert, Aitor Santamaría Merino, the FDOS Project, et al.
    Full text in kernel/COPYING.
  - **freecom/** — FreeCOM (COMMAND.COM), GPL-2.0. Authors: Steffen
    Kaiser, Aitor Santamaría Merino, et al. Full text in
    freecom/COPYING.
  - **pmodew/** — PMODE/W, Tran's DOS extender. Author: Charles
    "Tran" Scheffold and Thomas Pytel. License is "free for any use"
    as per the included notice file inside pmw${PMODEW_VER}.zip.
  - **bootdisk/** — packaged FreeDOS boot floppy from
    codercowboy/freedosbootdisks; same per-component licenses as
    the kernel and FreeCOM above.

The freedos_micro_python project itself is MIT-licensed; this
tarball is offered alongside it as voluntary source redistribution.
EOF

# ---------------------------------------------------------------------------
# Tar
# ---------------------------------------------------------------------------

echo "release: building tarball …"
( cd "$STAGE" && tar -czf "$OUT" freedos-sources/ )

SIZE_BYTES=$(stat -f%z "$OUT" 2>/dev/null || stat -c%s "$OUT")
SIZE_MB=$(( (SIZE_BYTES + 524288) / 1048576 ))
echo "release: wrote $OUT (${SIZE_MB} MB)"
