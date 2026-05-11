#!/usr/bin/env bash
# Build the standalone TLS smoke test.
#
# Iteration loop is ~98 sec end-to-end vs ~14 min for a full
# MicroPython rebuild — same lwIP+axtls+port code, but with all of
# MP's ~150 .c files dropped. Use this for fast debugging of any
# TLS/socket/cert path; once a fix lands here it can be lifted into
# `src/freedos_micro_python/port/` verbatim.
#
# Requirements (set the env vars when you run this, or pass values
# inline like `FMP_WORKDIR=… ./build.sh`):
#   FMP_WORKDIR    Build dir holding upstream/ and uc386-dos/ — the
#                  same workdir freedos-micropython fetch / port used.
#                  Defaults to /tmp/fmp_pypi_build (the convention
#                  used in CI + most local setups).
#   FMP_PY         Python interpreter that has uc386 + freedos_micro
#                  _python installed.
#   UC386_REPO     Path to the uc386 repo (for addons.harness.exe).
#                  Defaults to ../../../uc386 relative to this rig.
#   DOS32A_EXE     Path to DOS32A.EXE (zlib license; fetch from
#                  https://archive.org/details/dos32a-912-bin and
#                  unzip — the binary you want is binw/dos32a.exe).
#                  Required: PMODE/W's INT 21h AH=0x3F V86 reflection
#                  is broken (see rigs/dpmi-int21-smoke/ for proof),
#                  so the smoke binds DOS/32A as its extender.

set -eu

cd "$(dirname "$0")"
HERE="$(pwd)"

FMP_WORKDIR="${FMP_WORKDIR:-/tmp/fmp_pypi_build}"
UPSTREAM="$FMP_WORKDIR/upstream"

if [ ! -d "$UPSTREAM" ]; then
    echo "tls_smoke: $UPSTREAM/ missing." >&2
    echo "  Run 'freedos-micropython --workdir $FMP_WORKDIR fetch' first" >&2
    echo "  (or set FMP_WORKDIR=… to point at an existing build dir)" >&2
    exit 1
fi

PORT="$(cd ../../src/freedos_micro_python/port && pwd)"
UC386_REPO="${UC386_REPO:-$(cd ../../../uc386 && pwd)}"
FMP_PY="${FMP_PY:-$(command -v python3.14 || command -v python3.12 || command -v python3)}"
DOS32A_EXE="${DOS32A_EXE:-$FMP_WORKDIR/dos32a.exe}"

if [ ! -f "$DOS32A_EXE" ]; then
    echo "tls_smoke: DOS32A.EXE missing at $DOS32A_EXE." >&2
    echo "  Fetch + extract:" >&2
    echo "    curl -fsSL -o /tmp/dos32a.zip https://archive.org/download/dos32a-912-bin/dos32a-912-bin.zip" >&2
    echo "    unzip -j /tmp/dos32a.zip binw/dos32a.exe -d $FMP_WORKDIR" >&2
    exit 1
fi

UC386_LIB_INCLUDE=$("$FMP_PY" -c \
    "import uc386, pathlib; print(pathlib.Path(uc386.__file__).resolve().parent / 'lib' / 'include')")

mkdir -p build
SOURCES_FILE=build/_sources.txt
{
    # lwIP core
    echo "$UPSTREAM"/lib/lwip/src/core/init.c
    echo "$UPSTREAM"/lib/lwip/src/core/def.c
    echo "$UPSTREAM"/lib/lwip/src/core/dns.c
    echo "$UPSTREAM"/lib/lwip/src/core/inet_chksum.c
    echo "$UPSTREAM"/lib/lwip/src/core/raw.c
    echo "$UPSTREAM"/lib/lwip/src/core/ip.c
    echo "$UPSTREAM"/lib/lwip/src/core/mem.c
    echo "$UPSTREAM"/lib/lwip/src/core/memp.c
    echo "$UPSTREAM"/lib/lwip/src/core/netif.c
    echo "$UPSTREAM"/lib/lwip/src/core/pbuf.c
    echo "$UPSTREAM"/lib/lwip/src/core/stats.c
    echo "$UPSTREAM"/lib/lwip/src/core/sys.c
    echo "$UPSTREAM"/lib/lwip/src/core/tcp.c
    echo "$UPSTREAM"/lib/lwip/src/core/tcp_in.c
    echo "$UPSTREAM"/lib/lwip/src/core/tcp_out.c
    echo "$UPSTREAM"/lib/lwip/src/core/timeouts.c
    echo "$UPSTREAM"/lib/lwip/src/core/udp.c
    echo "$UPSTREAM"/lib/lwip/src/core/ipv4/dhcp.c
    echo "$UPSTREAM"/lib/lwip/src/core/ipv4/etharp.c
    echo "$UPSTREAM"/lib/lwip/src/core/ipv4/igmp.c
    echo "$UPSTREAM"/lib/lwip/src/core/ipv4/ip4.c
    echo "$UPSTREAM"/lib/lwip/src/core/ipv4/ip4_addr.c
    echo "$UPSTREAM"/lib/lwip/src/core/ipv4/ip4_frag.c
    echo "$UPSTREAM"/lib/lwip/src/core/ipv4/icmp.c
    echo "$UPSTREAM"/lib/lwip/src/netif/ethernet.c

    # axtls — same set MP pulls in
    echo "$UPSTREAM"/lib/axtls/ssl/asn1.c
    echo "$UPSTREAM"/lib/axtls/ssl/loader.c
    echo "$UPSTREAM"/lib/axtls/ssl/tls1.c
    echo "$UPSTREAM"/lib/axtls/ssl/tls1_clnt.c
    # tls1.c has a callsite for do_svr_handshake — pull tls1_svr.c to
    # satisfy the linker, even though the smoke only uses client.
    echo "$UPSTREAM"/lib/axtls/ssl/tls1_svr.c
    echo "$UPSTREAM"/lib/axtls/ssl/x509.c
    echo "$UPSTREAM"/lib/axtls/crypto/aes.c
    echo "$UPSTREAM"/lib/axtls/crypto/bigint.c
    echo "$UPSTREAM"/lib/axtls/crypto/crypto_misc.c
    echo "$UPSTREAM"/lib/axtls/crypto/hmac.c
    echo "$UPSTREAM"/lib/axtls/crypto/md5.c
    echo "$UPSTREAM"/lib/axtls/crypto/sha1.c
    # crypto-algorithms sha256.c is pulled in via `#include` from
    # tls_glue.c — see comment there. Compiling it standalone trips
    # a uc386 parse bug on `BYTE data[]` array params; we ship a
    # pointer-form patched copy under patches/.
    echo "$UPSTREAM"/lib/axtls/crypto/sha384.c
    echo "$UPSTREAM"/lib/axtls/crypto/sha512.c
    echo "$UPSTREAM"/lib/axtls/crypto/rsa.c

    # uc386-dos port glue. lwip_uc386dos.c brings the NE2000 netif +
    # the pktdrv recv/tx adapters; pktdrv_uc386dos.c is the PM-native
    # NE2000 driver (no DPMI 0x0303 needed); dosint21_uc386dos.c is
    # the DPMI 0x0301 → real-mode INT 21h thunk for file I/O.
    echo "$PORT"/lwip_uc386dos.c
    echo "$PORT"/pktdrv_uc386dos.c
    echo "$PORT"/uc386_net_uc386dos.c
    echo "$PORT"/dosint21_uc386dos.c
    echo "$PORT"/time_real_uc386dos.c

    # The smoke itself.
    echo ./main.c
    echo ./tls_glue.c
} > "$SOURCES_FILE"

N=$(wc -l < "$SOURCES_FILE" | tr -d ' ')
echo "tls_smoke: compiling $N sources via uc386 …"

# FORCE_CRYNWR=1 ./build.sh skips the PM-native NE2000 path and
# routes through the standard Crynwr-packet-driver + DPMI 0x0303
# flow — the path mTCP / htget use on FreeDOS. Lets us prove the
# port works against any NIC with a Crynwr driver, not just
# NE2000-at-0x300.
EXTRA_DEFS=""
if [ "${FORCE_CRYNWR:-0}" = "1" ]; then
    EXTRA_DEFS="$EXTRA_DEFS -DPKTDRV_FORCE_CRYNWR=1"
fi

tr '\n' '\0' < "$SOURCES_FILE" \
    | xargs -0 "$FMP_PY" -m uc386.main \
        -I "$UC386_LIB_INCLUDE" \
        -I "$UPSTREAM" \
        -I "$UPSTREAM"/lib/lwip/src/include \
        -I "$UPSTREAM"/extmod/lwip-include \
        -I "$UPSTREAM"/lib/axtls/ssl \
        -I "$UPSTREAM"/lib/axtls/crypto \
        -I "$UPSTREAM"/extmod/axtls-include \
        -I "$PORT" \
        -I "$FMP_WORKDIR"/uc386-dos \
        -I "$FMP_WORKDIR"/build \
        -D__linux__=1 \
        -DNDEBUG=1 \
        -DMICROPY_SSL_AXTLS=1 \
        -DMICROPY_PY_SSL=1 \
        -Dmp_stream_errno=errno \
        $EXTRA_DEFS \
        -o build/tls_smoke.asm
echo "tls_smoke: wrote build/tls_smoke.asm ($(wc -c < build/tls_smoke.asm | tr -d ' ') bytes)"

# Hand the combined .asm to addons.harness.exe (pyle under the hood)
# with DOS/32A as the extender — PMODE/W's INT 21h AH=0x3F V86
# reflection is broken (see rigs/dpmi-int21-smoke/), DOS/32A's works.
echo "tls_smoke: linking via pyle + DOS/32A …"
ASM_ABS="$HERE/build/tls_smoke.asm"
OUT_ABS="$HERE/build/TLS.EXE"
( cd "$UC386_REPO" && \
  "$FMP_PY" -m addons.harness.exe \
    "$ASM_ABS" \
    -o "$OUT_ABS" \
    --extender=dos32a \
    --stub-binary="$DOS32A_EXE" )
echo "tls_smoke: wrote build/TLS.EXE ($(wc -c < build/TLS.EXE | tr -d ' ') bytes)"
