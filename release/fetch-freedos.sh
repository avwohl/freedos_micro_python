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

# FreeDOS kernel — github.com/FDOS/kernel. Tags are `keNNNN` not
# `vN.N`; bump to the latest stable to roll forward.
KERNEL_TAG="ke2045"
KERNEL_URL="https://github.com/FDOS/kernel/archive/refs/tags/${KERNEL_TAG}.tar.gz"

# FreeCOM (COMMAND.COM) — github.com/FDOS/freecom. The repo doesn't
# carry stable tags so we pin a master SHA for reproducibility. Get
# the current head with:
#   curl -fsSL https://api.github.com/repos/FDOS/freecom/commits/master \
#     | python3 -c "import json,sys;print(json.load(sys.stdin)['sha'])"
FREECOM_SHA="ec6c63f13be0b254151c76cbbbec3c80ae33741b"   # 2025-10-30
FREECOM_URL="https://github.com/FDOS/freecom/archive/${FREECOM_SHA}.tar.gz"

# PMODE/W is Tran's DOS extender, bundled with FreeDOS but not
# FreeDOS-maintained — there is no stable canonical download URL.
# We try a couple of historical mirrors; if all fail we drop a
# note into the tarball with pointers and the uc386 stub binary
# we actually link against.
PMODEW_URLS=(
    "https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/devel/extender/pmodew/pmw133.zip"
    "https://www.fysnet.net/pmodew/pmw133.zip"
)
# The PMW stub binary uc386 actually links into MP.EXE. Always
# included in the tarball regardless of source-zip availability.
UC386_PMW_STUB="${UC386_PMW_STUB:-/Users/wohl/src/uc386/addons/harness/pmodew_stub.bin}"

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

echo "release: fetching FreeCOM @ ${FREECOM_SHA:0:12} …"
curl -fsSL "$FREECOM_URL" -o "$STAGE/freecom.tar.gz"
tar -xzf "$STAGE/freecom.tar.gz" -C "$ROOT/freecom" --strip-components=1

echo "release: fetching PMODE/W zip (best-effort) …"
pmw_ok=0
for url in "${PMODEW_URLS[@]}"; do
    if ! curl -fsSL "$url" -o "$ROOT/pmodew/pmw.zip" 2>/dev/null; then
        continue
    fi
    # Some mirrors return an HTML 404 page with status 200. Validate
    # that the bytes actually start with a ZIP "PK\x03\x04" magic.
    magic=$(head -c 4 "$ROOT/pmodew/pmw.zip" | od -A n -t x1 | tr -d ' \n')
    if [ "$magic" = "504b0304" ]; then
        echo "release: pmw.zip from $url ($(wc -c < "$ROOT/pmodew/pmw.zip" | tr -d ' ') bytes)"
        pmw_ok=1
        break
    else
        echo "release: $url returned non-zip bytes (magic=$magic) — skipping"
    fi
done
if [ "$pmw_ok" -eq 0 ]; then
    rm -f "$ROOT/pmodew/pmw.zip"
    cat > "$ROOT/pmodew/README.md" <<EOF
# PMODE/W source

The PMODE/W zip we usually ship here couldn't be fetched from any
of its historical mirrors at archive time. PMODE/W is Tran's
DOS extender (Charles "Tran" Scheffold + Thomas Pytel); it is
licensed "free for any use" but is not actively distributed by
the FreeDOS project itself.

To locate the current source/binary archive, search the FreeDOS
mailing-list archives and the FreeDOS forums for "PMODE/W 1.33"
or check archive.org snapshots of:

  - https://www.bttr-software.de/products/pmw/
  - https://www.fysnet.net/pmodew/
  - https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/devel/extender/pmodew/

The exact PMW stub binary that this project's MP.EXE links against
is in pmodew_stub.bin alongside this README.
EOF
fi
# Always include the stub binary uc386 actually uses.
if [ -f "$UC386_PMW_STUB" ]; then
    cp "$UC386_PMW_STUB" "$ROOT/pmodew/pmodew_stub.bin"
fi

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

  - **kernel/** (${KERNEL_TAG}) — FreeDOS kernel, GPL-2.0. Authors:
    Pat Villani, Tom Ehlert, Aitor Santamaría Merino, the FDOS
    Project, et al. Full text in kernel/COPYING.
  - **freecom/** (${FREECOM_SHA:0:12}) — FreeCOM (COMMAND.COM),
    GPL-2.0. Authors: Steffen Kaiser, Aitor Santamaría Merino, et
    al. Full text in freecom/COPYING.
  - **pmodew/** — PMODE/W, Tran's DOS extender. Author: Charles
    "Tran" Scheffold and Thomas Pytel. License is "free for any
    use" as per the bundled notice. See pmodew/README.md if the
    upstream zip was not retrievable at archive time; the stub
    binary uc386 links into MP.EXE is shipped as
    pmodew/pmodew_stub.bin.
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
