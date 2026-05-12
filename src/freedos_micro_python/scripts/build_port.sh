#!/bin/sh
# Multi-TU build of MicroPython through uc386 — produces a runnable
# `build/micropython.bin` that boots to the REPL banner under
# `uc386.dos_emu.run`.
#
# Sources:
#   - upstream/py/*.c                        (132)
#   - upstream/shared/libc/printf.c
#                       /string0.c
#                       /__errno.c
#                       /abort_.c
#   - upstream/shared/readline/readline.c
#   - upstream/shared/runtime/pyexec.c
#                            /stdout_helpers.c
#                            /interrupt_char.c
#                            /sys_stdio_mphal.c
#   - upstream/ports/minimal/main.c          (REPL driver)
#   - upstream/ports/minimal/uart_core.c     (mp_hal_stdin/stdout)
#
# Includes:
#   - lib/include                            (uc386 libc)
#   - upstream                               (py/, shared/)
#   - uc386-dos                              (mpconfigport.h, mphalport.h)
#   - build                                  (genhdr/qstrdefs.generated.h,
#                                             moduledefs.h, mpversion.h,
#                                             root_pointers.h — emitted
#                                             by ./build.sh during the
#                                             per-file triage pass)
#
# Defines:
#   -D__linux__=1                            triggers the minimal port's
#                                            MICROPY_MIN_USE_STDOUT path
#                                            (read(STDIN_FILENO)/write(...))
#                                            — uc386 lowers those into
#                                            INT 21h DOS syscalls.
#
# Wall-clock: ~14 minutes on the dev Mac (Apple Silicon, Python 3.12,
# uc386 in-process). Multi-TU is heavy because uc_core's preprocessor
# runs once per TU.
#
# Outputs:
#   build/micropython.asm    — NASM-syntax assembly (1.5 MB+)
#   build/micropython.bin    — flat 32-bit DOS bin (170 KB)
#
# Run:
#   .venv/bin/python -c "from pathlib import Path; \
#       from uc386.dos_emu import run; \
#       r = run(Path('addons/gnu/micropython/build/micropython.bin'), \
#               timeout_seconds=10.0); \
#       print(r.stdout)"
#
# The REPL banner ("MicroPython uc386-triage on ...; uc386-dos with i386")
# should appear, followed by `>>> ` and the boot waits on stdin.
set -eu


if [ ! -d upstream ]; then
    echo "micropython: run ./fetch.sh first." >&2
    exit 1
fi

# build.sh emits the genhdr/* stubs (qstrdefs.generated.h,
# moduledefs.h, mpversion.h, root_pointers.h) that py/*.c expects.
# Run it first if those aren't here yet.
if [ ! -f build/genhdr/qstrdefs.generated.h ]; then
    # Look for build.sh next to ourselves first (CLI sets
    # FREEDOS_MP_SCRIPTS_DIR; standalone caller likely has it as
    # a sibling of build_port.sh).
    BUILD_SH="${FREEDOS_MP_SCRIPTS_DIR:-$(dirname "$0")}/build.sh"
    if [ ! -x "$BUILD_SH" ]; then
        echo "micropython: cannot find build.sh; tried $BUILD_SH" >&2
        exit 1
    fi
    echo "micropython: stub headers missing; running build.sh first." >&2
    "$BUILD_SH" > /dev/null
fi

# Two ways this script gets its uc386 paths:
#  1. Invoked via `freedos-micropython port` — the Python CLI exports
#     UC386_LIB_INCLUDE (and FREEDOS_MP_PORT_DIR) from the installed
#     uc386 / freedos_micro_python packages, and pre-symlinks the
#     port files into cwd as ./uc386-dos.
#  2. Invoked standalone from a uc386 source checkout — falls back
#     to walking up to the uc386 repo root.
if [ -z "${UC386_LIB_INCLUDE:-}" ]; then
    REPO="$(cd ../../.. && pwd)"
    UC386_LIB_INCLUDE="$REPO/src/uc386/lib/include"
fi
if [ -n "${PYTHON:-}" ]; then
    :
elif [ -n "${REPO:-}" ] && [ -x "$REPO/.venv/bin/python" ]; then
    PYTHON="$REPO/.venv/bin/python"
else
    PYTHON="$(command -v python3.12 || command -v python3 || command -v python)"
fi
INCLUDE="$UC386_LIB_INCLUDE"

SOURCES_FILE="build/_port_sources.txt"
{
    find upstream/py -name '*.c' | sort
    echo upstream/shared/libc/__errno.c
    echo upstream/shared/libc/abort_.c
    echo upstream/shared/libc/printf.c
    echo upstream/shared/libc/string0.c
    echo upstream/shared/readline/readline.c
    echo upstream/shared/runtime/pyexec.c
    echo upstream/shared/runtime/stdout_helpers.c
    echo upstream/shared/runtime/interrupt_char.c
    echo upstream/shared/runtime/sys_stdio_mphal.c
    echo upstream/shared/timeutils/timeutils.c
    echo upstream/extmod/modtime.c
    echo upstream/extmod/moductypes.c
    echo upstream/extmod/modrandom.c
    echo upstream/extmod/modbinascii.c
    echo upstream/extmod/modhashlib.c
    echo upstream/extmod/modre.c
    echo upstream/extmod/modheapq.c
    # moddeflate.c inline-#includes all the uzlib sources
    # (tinflate.c, header.c, adler32.c, crc32.c, lz77.c → defl_static.c)
    # so don't add them to the source list — that would duplicate
    # symbols.
    echo upstream/extmod/moddeflate.c
    echo upstream/extmod/modjson.c
    echo upstream/extmod/modplatform.c
    echo upstream/extmod/modselect.c
    # modasyncio.c provides the _asyncio builtin module (TaskQueue +
    # Task primitives). The user-facing asyncio.* Python API lives in
    # upstream/extmod/asyncio/*.py — those still need to be shipped to
    # the DOS image as plain .py files (MICROPY_ENABLE_EXTERNAL_IMPORT
    # is on; frozen .mpy support is off) for `import asyncio` to work.
    # `import _asyncio` alone uses just this C module.
    echo upstream/extmod/modasyncio.c
    # `machine` — modmachine.c is the dispatch table, machine_mem.c
    # provides mem8/16/32 (the main DOS-useful feature, see
    # mpconfigport.h's MICROPY_PY_MACHINE block), machine_signal.c is
    # pulled in because MICROPY_PY_MACHINE_SIGNAL defaults to 1 when
    # MICROPY_PY_MACHINE is on. modmachine.c includes our port glue at
    # uc386-dos/modmachine_uc386dos.c via MICROPY_PY_MACHINE_INCLUDEFILE
    # (so the glue file doesn't appear in this source list).
    echo upstream/extmod/modmachine.c
    echo upstream/extmod/machine_mem.c
    echo upstream/extmod/machine_signal.c
    # machine_signal.c calls mp_virtual_pin_read/write — those live in
    # extmod/virtpin.c (the abstract Pin protocol any Pin-like object
    # plugs into). machine_signal works without a concrete Pin (you can
    # pass any object with a `value()` method) but the dispatch
    # functions still need to link.
    echo upstream/extmod/virtpin.c
    # MD5 + SHA1 are now from upstream/lib/axtls/crypto/ (added below
    # in the axtls section). B-Con's MD5 + SHA1 sources are still
    # fetched by fetch.sh's `fetch_b_con_crypto` hook so the legacy
    # shim at uc386-dos/lib/axtls/crypto/crypto.h still resolves
    # cleanly if you flip MICROPY_PY_SSL=0 + drop axtls from this
    # list, but they aren't compiled in the SSL build to avoid
    # carrying two MD5 implementations.
    # lwIP — IPv4-only TCP/UDP/DNS stack with loopback for testing.
    # Phase 1: just core + ipv4 + netif/ethernet. DHCP / IPv6 / SLIP /
    # PPP are off in lwipopts.h so their sources don't pull in.
    # axtls — real TLS via upstream/lib/axtls/. The MP glue is our
    # vendored fork at uc386-dos/modtls_axtls_uc386dos.c (which adds
    # real verify_mode + load_verify_locations on top of upstream's
    # minimal version). axtls library sources mirror upstream/extmod/
    # extmod.mk's AXTLS_DIR list.
    echo uc386-dos/modtls_axtls_uc386dos.c
    echo upstream/lib/axtls/ssl/asn1.c
    echo upstream/lib/axtls/ssl/loader.c
    echo upstream/lib/axtls/ssl/tls1.c
    echo upstream/lib/axtls/ssl/tls1_svr.c
    echo upstream/lib/axtls/ssl/tls1_clnt.c
    echo upstream/lib/axtls/ssl/x509.c
    echo upstream/lib/axtls/crypto/aes.c
    echo upstream/lib/axtls/crypto/bigint.c
    echo upstream/lib/axtls/crypto/crypto_misc.c
    echo upstream/lib/axtls/crypto/hmac.c
    echo upstream/lib/axtls/crypto/md5.c
    echo upstream/lib/axtls/crypto/sha1.c
    # SHA384/SHA512 — needed for cert chain verification when the CA
    # signs with those algorithms (RSA-SHA384 / RSA-SHA512 are common
    # on modern X.509 chains). Upstream MP's extmod.mk doesn't pull
    # these because their config has CONFIG_SSL_CERT_VERIFICATION
    # off; we have it on (uc386-dos has the room) so we need them.
    echo upstream/lib/axtls/crypto/sha384.c
    echo upstream/lib/axtls/crypto/sha512.c
    echo upstream/lib/axtls/crypto/rsa.c
    echo upstream/extmod/modlwip.c
    echo upstream/lib/lwip/src/core/init.c
    echo upstream/lib/lwip/src/core/def.c
    echo upstream/lib/lwip/src/core/dns.c
    echo upstream/lib/lwip/src/core/inet_chksum.c
    echo upstream/lib/lwip/src/core/ip.c
    echo upstream/lib/lwip/src/core/mem.c
    echo upstream/lib/lwip/src/core/memp.c
    echo upstream/lib/lwip/src/core/netif.c
    echo upstream/lib/lwip/src/core/pbuf.c
    echo upstream/lib/lwip/src/core/raw.c
    echo upstream/lib/lwip/src/core/stats.c
    echo upstream/lib/lwip/src/core/sys.c
    echo upstream/lib/lwip/src/core/tcp.c
    echo upstream/lib/lwip/src/core/tcp_in.c
    echo upstream/lib/lwip/src/core/tcp_out.c
    echo upstream/lib/lwip/src/core/timeouts.c
    echo upstream/lib/lwip/src/core/udp.c
    echo upstream/lib/lwip/src/core/ipv4/dhcp.c
    echo upstream/lib/lwip/src/core/ipv4/etharp.c
    echo upstream/lib/lwip/src/core/ipv4/icmp.c
    echo upstream/lib/lwip/src/core/ipv4/igmp.c
    echo upstream/lib/lwip/src/core/ipv4/ip4.c
    echo upstream/lib/lwip/src/core/ipv4/ip4_addr.c
    echo upstream/lib/lwip/src/core/ipv4/ip4_frag.c
    echo upstream/lib/lwip/src/netif/ethernet.c
    echo uc386-dos/lwip_uc386dos.c
    echo upstream/ports/minimal/main.c
    echo upstream/ports/minimal/uart_core.c
    echo uc386-dos/mphal_uc386dos.c
    echo uc386-dos/file_uc386dos.c
    echo uc386-dos/os_uc386dos.c
    echo uc386-dos/path_uc386dos.c
    echo uc386-dos/base64_uc386dos.c
    echo uc386-dos/shutil_uc386dos.c
    echo uc386-dos/tempfile_uc386dos.c
    echo uc386-dos/urllib_parse_uc386dos.c
    echo uc386-dos/urllib_uc386dos.c
    echo uc386-dos/uc386_net_uc386dos.c
    echo uc386-dos/pktdrv_uc386dos.c
    echo uc386-dos/dosint21_uc386dos.c
    echo uc386-dos/math_gamma.c
    # Real time(), mktime(), gettimeofday() backed by the DOS RTC —
    # replaces the libc.asm stubs (now removed). Required by axtls
    # cert verification (notBefore/notAfter window check).
    echo uc386-dos/time_real_uc386dos.c
} > "$SOURCES_FILE"

n_sources="$(wc -l < "$SOURCES_FILE")"
echo "micropython: compiling $n_sources sources via uc386 (multi-TU; ~14 min) …"

# tr+xargs delivery so the source list survives without arg-list overflow.
# `set -e` would abort on uc386 failure; we want to surface it explicitly.
set +e
tr '\n' '\0' < "$SOURCES_FILE" \
    | xargs -0 "$PYTHON" -m uc386.main \
        -I "$INCLUDE" \
        -I upstream \
        -I upstream/lib/lwip/src/include \
        -I upstream/extmod/lwip-include \
        -I upstream/lib/axtls/ssl \
        -I upstream/lib/axtls/crypto \
        -I upstream/extmod/axtls-include \
        -I uc386-dos \
        -I build \
        -D__linux__=1 \
        -DNDEBUG=1 \
        -DMICROPY_SSL_AXTLS=1 \
        -DMICROPY_PY_SSL=1 \
        -Dmp_stream_errno=errno \
        -o build/micropython.asm
rc=$?
set -e
if [ $rc -ne 0 ]; then
    echo "micropython: uc386 returned $rc; not assembling." >&2
    exit $rc
fi

asm_size="$(wc -c < build/micropython.asm | tr -d ' ')"
echo "micropython: wrote build/micropython.asm ($asm_size bytes)"
echo "micropython: assembling via nasm …"
# `-w-error=label-redef-late`: same convergence warning we suppress for
# DOOM. Long programs hit short/long jump promotion edge cases NASM 3.x
# now warns about; the binary's correct after convergence.
nasm -f bin -w-error=label-redef-late \
    build/micropython.asm -o build/micropython.bin
bin_size="$(wc -c < build/micropython.bin | tr -d ' ')"
echo "micropython: built build/micropython.bin ($bin_size bytes)"
ls -l build/micropython.bin
