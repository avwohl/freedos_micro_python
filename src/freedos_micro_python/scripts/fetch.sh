#!/bin/sh
# Fetch MicroPython upstream into upstream/.
# Idempotent: a second run is a no-op once upstream/ exists.
set -eu

# B-Con public-domain crypto reference impls used by hashlib's
# md5/sha1 (sha256 is already in upstream's tarball). Pull these
# even when upstream/ already exists, so an older fetch that
# pre-dates the crypto-algorithms additions self-heals on next run.
fetch_b_con_crypto() {
    CA_BASE="https://raw.githubusercontent.com/B-Con/crypto-algorithms/master"
    mkdir -p upstream/lib/crypto-algorithms
    for f in md5.c md5.h sha1.c sha1.h; do
        if [ ! -f "upstream/lib/crypto-algorithms/$f" ]; then
            echo "micropython: fetching crypto-algorithms/$f …"
            curl -fsSL "$CA_BASE/$f" -o "upstream/lib/crypto-algorithms/$f"
        fi
    done
}

# axtls — TLS library, submodule in upstream/.gitmodules pointing at
# micropython/axtls (a fork of axtls-2.1.x maintained for MP). Pinned
# to the SHA the upstream MP tarball references (gitlinks aren't pulled
# by the tarball, so we fetch a tree archive directly). The MP-extmod
# glue at upstream/extmod/modtls_axtls.c expects this tree at
# upstream/lib/axtls/.
fetch_axtls() {
    AXTLS_SHA="531cab9c278c947d268bd4c94ecab9153a961b43"
    AXTLS_DIR="upstream/lib/axtls"
    if [ -d "$AXTLS_DIR/ssl" ]; then
        return 0
    fi
    echo "micropython: fetching axtls $AXTLS_SHA …"
    AXTLS_TMP="$(mktemp -d)"
    trap 'rm -rf "$AXTLS_TMP"' RETURN 2>/dev/null || true
    curl -fsSL \
        "https://github.com/micropython/axtls/archive/${AXTLS_SHA}.tar.gz" \
        -o "$AXTLS_TMP/axtls.tgz"
    tar -xzf "$AXTLS_TMP/axtls.tgz" -C "$AXTLS_TMP"
    mkdir -p "$AXTLS_DIR"
    cp -r "$AXTLS_TMP"/axtls-*/ssl    "$AXTLS_DIR/"
    cp -r "$AXTLS_TMP"/axtls-*/crypto "$AXTLS_DIR/"
    rm -rf "$AXTLS_TMP"
}

# lwIP — submodule in upstream/.gitmodules but not pulled by the
# tarball. Fetch a pinned release tarball into upstream/lib/lwip/
# alongside the existing crypto-algorithms stash. STABLE-2_2_1 is
# the latest tagged release as of mid-2026 and is what our port
# integration targets.
fetch_lwip() {
    LWIP_TAG="STABLE-2_2_1_RELEASE"
    LWIP_DIR="upstream/lib/lwip"
    if [ -d "$LWIP_DIR/src/core" ]; then
        return 0
    fi
    echo "micropython: fetching lwIP $LWIP_TAG …"
    LWIP_TMP="$(mktemp -d)"
    trap 'rm -rf "$LWIP_TMP"' RETURN 2>/dev/null || true
    curl -fsSL \
        "https://github.com/lwip-tcpip/lwip/archive/refs/tags/${LWIP_TAG}.tar.gz" \
        -o "$LWIP_TMP/lwip.tgz"
    tar -xzf "$LWIP_TMP/lwip.tgz" -C "$LWIP_TMP"
    mkdir -p "$LWIP_DIR"
    cp -r "$LWIP_TMP"/lwip-*/src "$LWIP_DIR/"
    rm -rf "$LWIP_TMP"
}

# Patch upstream's `mod_lwip_reset` to register our loopback packet
# pump as the poll callback (instead of nulling it). LWIP_NETIF_LOOPBACK=1
# with NO_SYS=1 needs manual netif_poll(netif_default) per tick to
# deliver packets; uc386dos_loopback_poll (in uc386-dos/lwip_uc386dos.c)
# does that. Idempotent: skips when the patched line is already present.
patch_modlwip_loopback_poll() {
    F="upstream/extmod/modlwip.c"
    if [ ! -f "$F" ]; then return 0; fi
    if grep -q "uc386dos_loopback_poll" "$F"; then return 0; fi
    if ! grep -q "lwip_poll_list.poll = NULL;" "$F"; then
        echo "micropython: warn: modlwip.c reset shape changed — skipping loopback patch." >&2
        return 0
    fi
    echo "micropython: patching modlwip.c mod_lwip_reset for loopback poll …"
    awk '
        /static mp_obj_t mod_lwip_reset/ { in_reset = 1 }
        in_reset && /lwip_poll_list\.poll = NULL;/ {
            print "    extern void uc386dos_loopback_poll(void *arg);"
            print "    lwip_poll_list.poll = uc386dos_loopback_poll;"
            print "    lwip_poll_list.poll_arg = NULL;"
            in_reset = 0
            next
        }
        in_reset && /^}/ { in_reset = 0 }
        { print }
    ' "$F" > "$F.tmp" && mv "$F.tmp" "$F"
}

# Inject write(1, ...) checkpoints into ports/minimal/main.c so the
# rig can see how far MP startup gets. write() goes through libc's
# INT 21h AH=0x40 BX=1 — which respects DOS file-handle redirection
# (the > MP_OUT.TXT in autoexec.bat). printf() uses INT 21h AH=02h
# which writes to the console regardless of redirect, so under
# `dosbox-x -silent` it lands in /dev/null. The MP REPL itself uses
# mp_hal_stdout_tx_strn -> write(STDOUT_FILENO, ...), so these
# markers exercise the same redirect path as the MP banner. The
# last-seen marker in RIG.LOG identifies the statement that
# silently aborts. Idempotent: skip if the markers are already in
# place.
patch_main_startup_markers() {
    F="upstream/ports/minimal/main.c"
    if [ ! -f "$F" ]; then return 0; fi
    if grep -q "mp-startup-marker" "$F"; then
        # Old printf-based markers from a prior fetch.sh? Strip them
        # so the new write()-based markers are what gets compiled.
        if grep -q "printf(\"\[mp-" "$F"; then
            echo "micropython: stripping stale printf startup markers from ports/minimal/main.c …"
            grep -v "mp-startup-marker\|printf(\"\[mp-\|fflush(stdout)" "$F" > "$F.tmp" && mv "$F.tmp" "$F"
        else
            return 0
        fi
    fi
    echo "micropython: patching ports/minimal/main.c with write() startup markers …"
    awk '
        /^#include "shared\/runtime\/pyexec.h"/ {
            print
            print "#include <unistd.h>  /* mp-startup-marker: write() */"
            next
        }
        /^int main\(int argc, char \*\*argv\) \{/ {
            print
            print "    /* mp-startup-marker injected by addons/gnu/micropython/fetch.sh */"
            print "    write(1, \"[mp-main-entered]\\n\", 18);"
            in_main = 1
            next
        }
        in_main && /mp_stack_ctrl_init\(\);/ {
            print "    write(1, \"[mp-before-stack-ctrl]\\n\", 23);"
            print
            next
        }
        in_main && /mp_init\(\);/ {
            print "    write(1, \"[mp-before-mp-init]\\n\", 20);"
            print
            print "    write(1, \"[mp-after-mp-init]\\n\", 19);"
            next
        }
        in_main && /pyexec_friendly_repl\(\);/ {
            print "    write(1, \"[mp-before-repl]\\n\", 17);"
            print
            print "    write(1, \"[mp-after-repl]\\n\", 16);"
            next
        }
        in_main && /mp_deinit\(\);/ {
            print "    write(1, \"[mp-before-deinit]\\n\", 19);"
            print
            in_main = 0
            next
        }
        { print }
    ' "$F" > "$F.tmp" && mv "$F.tmp" "$F"
}

# Patch upstream/ports/minimal/main.c to pre-allocate the pktdrv bounce
# buffer and INT 0x60 real-mode thunk segment EARLY in main() — before
# MicroPython grows the C stack. Background:
#
#   * PMODE/W's INT 21h AH=0x48 (DOS Allocate Memory) deterministically
#     hangs when called from a deep stack frame. The MicroPython REPL +
#     interpreter pushes us well past the depth where this triggers, so
#     by the time pktdrv_init wants a real-mode bounce buffer the
#     allocation never returns. Allocating it from the top of main()
#     while the stack is shallow sidesteps the bug.
#   * The INT 0x60 real-mode thunk paragraph that pktdrv_call_int60_thunk
#     uses (DPMI fn 0x0301 dispatch) MUST be allocated via DPMI fn 0x0100
#     (DPMI Allocate DOS Memory), not INT 21h AH=0x48. PMODE/W's DPMI
#     host hangs when 0x0301 dispatches to a segment it doesn't know
#     about. 0x0100 makes the host record the allocation; 0x0301 then
#     works.
#
# Pktdrv code (port/pktdrv_uc386dos.c) reads the three globals
# `pktdrv_preallocated_bounce_seg`, `pktdrv_preallocated_bounce_linear`,
# and `pktdrv_preallocated_thunk_seg` and skips its own AH=0x48 path
# when they're non-zero. Idempotent: skips if the globals are already
# declared in the file.
patch_main_pktdrv_prealloc() {
    F="upstream/ports/minimal/main.c"
    if [ ! -f "$F" ]; then return 0; fi
    if grep -q "pktdrv_preallocated_bounce_seg" "$F"; then
        return 0
    fi
    echo "micropython: patching ports/minimal/main.c with pktdrv bounce + thunk pre-alloc …"
    awk '
        # Inject globals + helper just before int main(...). Keying on
        # the "int main(" line keeps us robust to whether the startup-
        # marker patch has already injected its [mp-main-entered] line
        # — we always land in the right spot.
        /^int main\(int argc, char \*\*argv\) \{/ && !inserted {
            print "// PMODE/W'\''s INT 21h AH=0x48 (Allocate Memory) deterministically hangs"
            print "// when called from a deep stack frame (empirically: 400+ bytes of"
            print "// extra locals on top of MicroPython'\''s interpreter stack triggers"
            print "// it; the unbloated baseline works). Workaround: pre-allocate the"
            print "// bounce buffer early in main(), before MicroPython builds up its"
            print "// stack depth, and store it in a global the pktdrv code reads."
            print "extern unsigned char pktdrv_int_invoke(unsigned int, unsigned int *);"
            print "unsigned int pktdrv_preallocated_bounce_seg = 0;"
            print "unsigned int pktdrv_preallocated_bounce_linear = 0;"
            print "unsigned int pktdrv_preallocated_thunk_seg = 0;"
            print "// Second pre-allocated paragraph for the INT 21h thunk used by"
            print "// port/dosint21_uc386dos.c. Same shallow-stack workaround — needs"
            print "// to be a DPMI fn 0x0100 alloc so DPMI 0x0301 can dispatch to it."
            print "unsigned int dos_int21_preallocated_thunk_seg = 0;"
            print ""
            print "static void _preallocate_bounce_buffer(void) {"
            print "    write(1, \"[bounce:pre-alloc]\\n\", 19);"
            print "    unsigned int regs[8] = {0};"
            print "    regs[0] = 0x4800;       // AH=0x48 Allocate Memory"
            print "    regs[1] = 128;          // 128 paragraphs = 2 KB"
            print "    unsigned char carry = pktdrv_int_invoke(0x21, regs);"
            print "    write(1, \"[bounce:post-alloc]\\n\", 20);"
            print "    if (carry) return;"
            print "    unsigned int seg = regs[0] & 0xFFFF;"
            print "    pktdrv_preallocated_bounce_seg = seg;"
            print ""
            print "    // PMODE/W uses paging — `seg << 4` is NOT the flat-32 linear"
            print "    // address that maps the conventional memory at that real-mode"
            print "    // segment. We need to ask DPMI for a selector that maps the"
            print "    // segment, then read its linear base."
            print "    //"
            print "    //   DPMI fn 0x0002: allocate descriptor for real-mode segment"
            print "    //     in: AX=0x0002, BX=segment   out: AX=selector"
            print "    //   DPMI fn 0x0006: get selector base address"
            print "    //     in: AX=0x0006, BX=selector  out: CX=base_high, DX=base_low"
            print "    unsigned int r2[8] = {0};"
            print "    r2[0] = 0x0002;"
            print "    r2[1] = seg;"
            print "    pktdrv_int_invoke(0x31, r2);"
            print "    unsigned int sel = r2[0] & 0xFFFF;"
            print "    unsigned int r3[8] = {0};"
            print "    r3[0] = 0x0006;"
            print "    r3[1] = sel;"
            print "    pktdrv_int_invoke(0x31, r3);"
            print "    unsigned int base_high = r3[2] & 0xFFFF;   // CX"
            print "    unsigned int base_low  = r3[3] & 0xFFFF;   // DX"
            print "    pktdrv_preallocated_bounce_linear = (base_high << 16) | base_low;"
            print ""
            print "    // Pre-allocate 5 paragraphs (80 bytes) for the INT 0x60 thunk."
            print "    // First 16 bytes are the INT 0x60 trampoline + type filter"
            print "    // bytes; remaining 64 bytes hold a real-mode RX receiver stub"
            print "    // + scratch words that bypass the DPMI fn 0x0303 callback path"
            print "    // (DOS/32A'\''s 0x0303 dispatch corrupts state under NE2000.COM'\''s"
            print "    // IRQ-context FAR CALL -- see pktdrv_alloc_thunk in"
            print "    // pktdrv_uc386dos.c)."
            print "    // Use DPMI fn 0x0100 (not INT 21h AH=0x48) so the DPMI host"
            print "    // registers this segment as one it knows about — DPMI 0x0301"
            print "    // dispatch to a non-DPMI-allocated segment hangs on PMODE/W."
            print "    write(1, \"[thunk:pre-alloc]\\n\", 18);"
            print "    unsigned int rt[8] = {0};"
            print "    rt[0] = 0x0100;          // AX=0x0100 (DPMI Allocate DOS Memory)"
            print "    rt[1] = 5;               // BX = 5 paragraphs (80 bytes total)"
            print "    unsigned char tcarry = pktdrv_int_invoke(0x31, rt);"
            print "    write(1, \"[thunk:post-alloc]\\n\", 19);"
            print "    if (!tcarry) {"
            print "        pktdrv_preallocated_thunk_seg = rt[0] & 0xFFFF;"
            print "    }"
            print ""
            print "    // Second 1-paragraph alloc for the INT 21h thunk."
            print "    // port/dosint21_uc386dos.c writes CD 21 CB here and"
            print "    // dispatches via DPMI 0x0301 to bypass PMODE/W'\''s broken"
            print "    // INT 21h V86 reflection."
            print "    write(1, \"[i21thunk:pre-alloc]\\n\", 21);"
            print "    unsigned int rt2[8] = {0};"
            print "    rt2[0] = 0x0100;"
            print "    rt2[1] = 1;"
            print "    unsigned char t2carry = pktdrv_int_invoke(0x31, rt2);"
            print "    write(1, \"[i21thunk:post-alloc]\\n\", 22);"
            print "    if (!t2carry) {"
            print "        dos_int21_preallocated_thunk_seg = rt2[0] & 0xFFFF;"
            print "    }"
            print "}"
            print ""
            inserted = 1
            in_main = 1
            print
            next
        }
        # Inject the call to _preallocate_bounce_buffer() right after
        # `stack_top = (char *)&stack_dummy;` — that is the first real
        # statement inside main(), so the stack is at its shallowest.
        in_main && /stack_top = \(char \*\)&stack_dummy;/ {
            print
            print ""
            print "    // Pre-allocate the pktdrv bounce buffer NOW, while the stack is"
            print "    // shallow. PMODE/W'\''s AH=0x48 hangs from deep stacks."
            print "    _preallocate_bounce_buffer();"
            in_main = 0
            next
        }
        { print }
    ' "$F" > "$F.tmp" && mv "$F.tmp" "$F"
}

# Disable upstream/ports/minimal/main.c's stub `mp_lexer_new_from_file`
# and `mp_import_stat`. The minimal port ships them as no-op
# OSError/NO_EXIST stubs because it has no filesystem; our port DOES
# have one (`port/file_uc386dos.c` defines real implementations
# backed by INT 21h). Without the rename we get duplicate-symbol
# link errors — or worse, the stubs win and `import foo` /
# `open()` silently fail. Renaming to `_unused_*` defangs the
# minimal port copies while keeping the file otherwise intact.
# Idempotent: skips if the rename has already been applied.
patch_main_disable_fs_stubs() {
    F="upstream/ports/minimal/main.c"
    if [ ! -f "$F" ]; then return 0; fi
    if grep -q "_unused_mp_lexer_new_from_file" "$F"; then
        return 0
    fi
    if ! grep -q "^mp_lexer_t \*mp_lexer_new_from_file(qstr filename)" "$F"; then
        echo "micropython: warn: ports/minimal/main.c FS stub shape changed — skipping rename." >&2
        return 0
    fi
    echo "micropython: renaming ports/minimal/main.c FS stubs to _unused_* …"
    sed -i.bak \
        -e 's|^mp_lexer_t \*mp_lexer_new_from_file(qstr filename) {|static mp_lexer_t *_unused_mp_lexer_new_from_file(qstr filename) { (void)filename;|' \
        -e 's|^mp_import_stat_t mp_import_stat(const char \*path) {|static mp_import_stat_t _unused_mp_import_stat(const char *path) { (void)path;|' \
        "$F"
    rm -f "$F.bak"
}

# Patch upstream/extmod/axtls-include/config.h to flip CONFIG_SSL_HAS_PEM
# and CONFIG_SSL_CERT_VERIFICATION from undef to defined. Upstream MP
# ports compile axtls in skeleton mode without verification because
# CA bundles don't fit on most embedded targets; uc386-dos has the
# room and wants real authentication. The flips engage axtls's
# x509_dn_compare / signature checking / notBefore-notAfter window
# checks and make `ssl.SSLContext.load_verify_locations` meaningful.
# Idempotent: leaves the flips in place once applied.
patch_axtls_config_verify() {
    F="upstream/extmod/axtls-include/config.h"
    if [ ! -f "$F" ]; then return 0; fi
    if grep -q "^#define CONFIG_SSL_CERT_VERIFICATION 1$" "$F"; then
        return 0
    fi
    echo "micropython: enabling axtls cert verification + PEM in $F …"
    sed -i.bak \
        -e 's|^#undef CONFIG_SSL_HAS_PEM$|#define CONFIG_SSL_HAS_PEM 1|' \
        -e 's|^#undef CONFIG_SSL_CERT_VERIFICATION$|#define CONFIG_SSL_CERT_VERIFICATION 1|' \
        "$F"
    rm -f "$F.bak"
}

# Add `#include <endian.h>` at the top of upstream/extmod/axtls-include/
# axtls_os_port.h. axtls's sha512.c uses be64toh() but doesn't include
# the header itself — on Linux glibc's <sys/time.h> transitively pulls
# in endian.h, on uc386's libc it doesn't. Adding the include via
# axtls_os_port.h (which every axtls TU pulls through os_port.h) is
# the minimal-blast-radius fix. Idempotent.
patch_axtls_endian_include() {
    F="upstream/extmod/axtls-include/axtls_os_port.h"
    if [ ! -f "$F" ]; then return 0; fi
    if grep -q "<endian.h>" "$F"; then
        return 0
    fi
    echo "micropython: adding endian.h include to $F …"
    sed -i.bak \
        's|^#include <errno.h>$|#include <errno.h>\
#include <endian.h>|' \
        "$F"
    rm -f "$F.bak"
}

# axtls's `get_random` (upstream/lib/axtls/crypto/crypto_misc.c) calls
# `gettimeofday(&tv, NULL)` on the non-Linux/non-Windows fallback path.
# Under FreeDOS+DOS/32A, the first call works but the second-and-later
# calls hit an INT 21h re-entrancy hang inside the AH=0x2A handler.
# The hang surfaces during the TLS handshake (the call from
# `send_client_key_xchg` hangs DOS).  Patch out the gettimeofday call
# and seed `tv` from a self-incrementing 64-bit counter instead.  The
# RNG output is only used for one-shot per-session secrets
# (pre-master, ClientHello random) — not long-lived key material —
# so the weaker entropy is acceptable for this port.  Idempotent.
patch_axtls_get_random_dos_int21() {
    F="upstream/lib/axtls/crypto/crypto_misc.c"
    if [ ! -f "$F" ]; then return 0; fi
    if grep -q "uc386-dos: skip gettimeofday" "$F"; then
        return 0
    fi
    if ! grep -q "gettimeofday(&tv, NULL);" "$F"; then
        echo "micropython: warn: crypto_misc.c shape changed — skipping gettimeofday patch." >&2
        return 0
    fi
    echo "micropython: patching axtls get_random to skip INT 21h gettimeofday …"
    awk '
        /gettimeofday\(&tv, NULL\);/ {
            print "    /* uc386-dos: skip gettimeofday() — see fetch.sh */"
            print "    {"
            print "        static unsigned long _rng_counter = 0;"
            print "        _rng_counter += 0x9E3779B97F4A7C15UL;"
            print "        tv.tv_sec = (long)(_rng_counter & 0x7FFFFFFFUL);"
            print "        tv.tv_usec = (long)((_rng_counter >> 16) & 0x7FFFFFFFUL);"
            print "    }"
            next
        }
        { print }
    ' "$F" > "$F.tmp" && mv "$F.tmp" "$F"
}

if [ -d upstream ]; then
    echo "micropython: upstream/ already present — skipping main fetch."
    fetch_b_con_crypto
    fetch_lwip
    fetch_axtls
    patch_modlwip_loopback_poll
    patch_main_startup_markers
    patch_main_pktdrv_prealloc
    patch_main_disable_fs_stubs
    patch_axtls_config_verify
    patch_axtls_endian_include
    patch_axtls_get_random_dos_int21
    exit 0
fi

# Pinned to a specific upstream commit so the source we ship in the
# tarball alongside the binary always matches what was built. The
# port currently targets v1.26-era APIs; bump with
# `git ls-remote https://github.com/micropython/micropython HEAD`
# and re-run the build cycle when promoting to a newer revision.
SHA="9f396bba8d675ffb53f7fb047def21c7a581948e"  # 2025-08-01
URL="https://github.com/micropython/micropython/archive/${SHA}.tar.gz"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "micropython: fetching $URL …"
curl -fsSL "$URL" -o "$TMP/micropython.tar.gz"
tar -xzf "$TMP/micropython.tar.gz" -C "$TMP"
mv "$TMP"/micropython-* upstream
echo "micropython: upstream tree at $(pwd)/upstream/"

fetch_b_con_crypto
fetch_lwip
fetch_axtls
patch_modlwip_loopback_poll
patch_main_startup_markers
patch_main_pktdrv_prealloc
patch_main_disable_fs_stubs
patch_axtls_config_verify
patch_axtls_endian_include
patch_axtls_get_random_dos_int21
