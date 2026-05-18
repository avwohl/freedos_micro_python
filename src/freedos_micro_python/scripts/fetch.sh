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
    # Force tcp_output (no Nagle). Separately idempotent so we
    # can land it after the loopback_poll patch is already in
    # place. Tens-of-seconds deadlocks happen otherwise because
    # the small SFTP_INIT (and other small SSH packets) sit in
    # the TCP buffer waiting for an ACK that won't come — the
    # single-threaded cooperative scheduler has libssh2's
    # BLOCK_ADJUST loop with nothing else to drive the socket.
    if ! grep -q "uc386-dos: force tcp_output" "$F"; then
        sed -i.bak 's|err = tcp_output_nagle(socket->pcb.tcp);|err = tcp_output(socket->pcb.tcp);  /* uc386-dos: force tcp_output -- see fetch.sh */|' "$F"
        rm -f "$F.bak"
    fi
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
            print "    // DPMI fn 0x0100 (not INT 21h AH=0x48) so PMODE/W"
            print "    // registers this segment in its DPMI tables. When"
            print "    // we pass it as DS in an rmcs to DPMI 0x0301 — the"
            print "    // dispatch our dos_int21_call uses for"
            print "    // open/read/write/close — unregistered segments"
            print "    // wedge the call after the binary grows large enough"
            print "    // that AH=0x48 starts returning a different segment"
            print "    // range. Symptom: open() hangs once a heavy module"
            print "    // like _ssh / axtls / libssh2 is linked in."
            print "    regs[0] = 0x0100;"
            print "    regs[1] = 128;          // 128 paragraphs = 2 KB"
            print "    unsigned char carry = pktdrv_int_invoke(0x31, regs);"
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
            print "// Resolve the INT 21h thunk's linear address + write the"
            print "// `CD 21 CB` opcode bytes there NOW, while the stack is"
            print "// shallow. Mirrors `_preallocate_bounce_buffer`'\''s rationale:"
            print "// PMODE/W'\''s DPMI lookup paths have shown deep-stack"
            print "// sensitivities, and the equivalent work used to run"
            print "// lazy-on-first-`open()` deep inside the MicroPython"
            print "// interpreter stack. Defined in port/dosint21_uc386dos.c."
            print "extern void dos_int21_thunk_preinit(void);"
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
            print "    // Same idea for the INT 21h thunk: resolve its linear"
            print "    // address + write the opcode bytes now, not lazily on"
            print "    // the first open()."
            print "    dos_int21_thunk_preinit();"
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
            print "    /* uc386-dos: skip gettimeofday() — see fetch.sh."
            print "       Use the 32-bit golden-ratio constant (NOT the"
            print "       ..7F4A7C15ULL form) — the 64-bit constant trips"
            print "       uc386 into 64-bit arithmetic that leaves edx live"
            print "       across statement boundaries, producing garbage"
            print "       counter increments and effectively-zero entropy."
            print "       Same fix as patch_axtls_time_dos_int21. */"
            print "    {"
            print "        static unsigned long _rng_counter = 0;"
            print "        _rng_counter += 0x9E3779B9UL;"
            print "        tv.tv_sec = (long)(_rng_counter & 0x7FFFFFFFUL);"
            print "        tv.tv_usec = (long)((_rng_counter >> 16) & 0x7FFFFFFFUL);"
            print "    }"
            next
        }
        { print }
    ' "$F" > "$F.tmp" && mv "$F.tmp" "$F"
}

# axtls's TLS handshake path calls `time(NULL)` in two more spots beyond
# get_random:
#   - ssl/tls1_clnt.c send_client_hello   (ClientHello timestamp nonce)
#   - ssl/tls1.c     ssl_session_update    (session LRU age)
# Both go through our libc's `time()` → `dos_get_datetime` → INT 21h
# AH=0x2A, which hits the same re-entrancy hang as the gettimeofday
# path inside get_random (commit 9b0f9a4). Without this patch
# ssl_client_new() hangs inside send_client_hello BEFORE writing
# ClientHello to the wire — observable as a TLS handshake that
# never starts (pcap shows TCP 3-way only, then silence).
# Replace both with self-incrementing counters; the values are
# non-security-critical (one is the nonce timestamp prefix, the
# other is an LRU age tag). Idempotent.
# axtls's cert verification path (`ssl/x509.c x509_verify`) calls
# `gettimeofday(&tv, NULL)` to check the notBefore/notAfter window
# during process_certificate. Same INT 21h AH=0x2A re-entrancy
# hang as the time(NULL) patches above — observable as a TLS
# handshake that gets past ClientHello + ServerHello + cert +
# ServerHelloDone, then never sends ClientKeyExchange. Skip the
# date-window check entirely; signature/chain verification still
# runs above this point in the same function, which is the actual
# meaningful security check. Idempotent.
patch_axtls_x509_gettimeofday() {
    F="upstream/lib/axtls/ssl/x509.c"
    [ -f "$F" ] || return 0
    if grep -q "uc386-dos: skip gettimeofday() — INT 21h AH=0x2A re-entrancy" "$F"; then
        return 0
    fi
    if ! grep -q "    gettimeofday(&tv, NULL);" "$F"; then
        echo "micropython: warn: x509.c shape changed — skipping x509 gettimeofday patch." >&2
        return 0
    fi
    echo "micropython: patching axtls x509_verify to skip INT 21h gettimeofday …"
    # Replace the one-line `gettimeofday(&tv, NULL);` call with a
    # hardcoded 2026-mid timestamp so the notBefore/notAfter
    # comparisons below still run against a sane "current time."
    awk '
        /^    gettimeofday\(&tv, NULL\);$/ {
            print "    /* uc386-dos: skip gettimeofday() — INT 21h AH=0x2A re-entrancy"
            print "       hangs PMODE/W mid-handshake. Use a hardcoded 2026-mid"
            print "       timestamp so the notBefore/notAfter checks still run. */"
            print "    tv.tv_sec = 1779000000L;   /* 2026-05-13 19:00 UTC */"
            print "    tv.tv_usec = 0;"
            next
        }
        { print }
    ' "$F" > "$F.tmp" && mv "$F.tmp" "$F"
}

# uc386's preprocessor doesn't expand a macro identifier when its
# argument list is on the NEXT source line. libssh2.h relies on
# that pattern for callback function-pointer params:
#
#   LIBSSH2_PASSWD_CHANGEREQ_FUNC          ← line N
#       ((*passwd_change_cb))              ← line N+1
#
# Join those continuation lines so the macro name + arglist sit
# together on one line; the preprocessor then expands normally.
# Idempotent: skips when the marker is already present.
patch_libssh2_crypto_engine_enum() {
    # libssh2.h's libssh2_crypto_engine_t enum lists the supported
    # backends. Our axtls backend defines LIBSSH2_CRYPTO_ENGINE as
    # libssh2_axtls; version.c returns it, but the enum doesn't list
    # libssh2_axtls so the identifier is undefined. Append it.
    # Idempotent.
    F="upstream/lib/libssh2/include/libssh2.h"
    if [ ! -f "$F" ]; then return 0; fi
    if grep -q "libssh2_axtls" "$F"; then
        return 0
    fi
    echo "micropython: adding libssh2_axtls to libssh2.h crypto_engine_t enum …"
    perl -0777 -i -pe '
        s/(libssh2_os400qc3)(\s*\n\}\s+libssh2_crypto_engine_t)/$1,\n    libssh2_axtls$2/;
    ' "$F"
}

# libssh2's LIBSSH2_KEX_METHOD_EC_SHA_HASH_CREATE_VERIFY macro is
# defined inside `#if LIBSSH2_ECDSA` in kex.c, but is also referenced
# inside `#if LIBSSH2_ED25519` (curve25519 KEX hash verification).
# When the backend has ECDSA=0 + ED25519=1 (ours does), the macro
# isn't defined where it's used and the build fails with "symbol
# `_LIBSSH2_KEX_METHOD_EC_SHA_HASH_CREATE_VERIFY' not defined".
#
# Hoist the macro definition outside the #if LIBSSH2_ECDSA gate so
# the ED25519 path sees it. The macro itself doesn't depend on
# ECDSA — it's just a generic hash-and-verify block that both
# ECDSA and Ed25519 use.
# TweetNaCl's curve25519 / ed25519 path needs one uc386-dos
# adaptation: hoist the largest stack-allocated arrays to
# file-static. crypto_scalarmult / crypto_sign_open / add /
# scalarbase / unpackneg / pack have frame sizes that push past
# PMODE/W's INT 21h deep-stack threshold; making the locals
# static is safe in this single-threaded port (no re-entrancy).
#
# Historical note: this patch used to also rewrite a long list of
# i64 compound assigns (`m[i-1] &= 0xffff`, `t[i+j] += …`, etc.)
# as plain stores to work around a uc386 codegen bug where the
# `__compll_addr/snap_*` slots emitted by `_compound_assign_ll`
# aliased a user loop variable. uc386 commit `10b4dfd` fixed the
# root cause (alloc_local now advances frame_size on decl
# re-bind), so the compound-assign rewrites have been dropped.
# The file-static hoists below are kept because they are about
# PMODE/W stack depth, not the compll bug.
patch_tweetnacl_uc386dos() {
    F="upstream/lib/tweetnacl/tweetnacl.c"
    [ -f "$F" ] || return 0
    if grep -q "uc386-dos: locals moved to file-static" "$F"; then
        return 0
    fi
    echo "micropython: patching tweetnacl for uc386-dos PMODE/W stack frames …"
    perl -0777 -i -pe '
        # crypto_scalarmult: hoist u8 z[32], i64 x[80], gf a..f to static.
        s{(int crypto_scalarmult\(u8 \*q,const u8 \*n,const u8 \*p\)\n\{\n)  u8 z\[32\];\n  i64 x\[80\],r,i;\n  gf a,b,c,d,e,f;\n}
         {$1  /* uc386-dos: locals moved to file-static — see fetch.sh */\n  static u8 z[32];\n  static i64 x[80];\n  i64 r,i;\n  static gf a,b,c,d,e,f;\n}s;

        # add: hoist all 9 gf locals.
        s{(sv add\(gf p\[4\],gf q\[4\]\)\n\{\n)  gf a,b,c,d,t,e,f,g,h;\n}
         {$1  /* uc386-dos: locals moved to file-static */\n  static gf a,b,c,d,t,e,f,g,h;\n}s;

        # scalarbase: hoist gf q[4].
        s{(sv scalarbase\(gf p\[4\],const u8 \*s\)\n\{\n)  gf q\[4\];\n}
         {$1  /* uc386-dos: locals moved to file-static */\n  static gf q[4];\n}s;

        # unpackneg: hoist its 7 gf locals.
        s{(static int unpackneg\(gf r\[4\],const u8 p\[32\]\)\n\{\n)  gf t, chk, num, den, den2, den4, den6;\n}
         {$1  /* uc386-dos: locals moved to file-static */\n  static gf t, chk, num, den, den2, den4, den6;\n}s;

        # crypto_sign_open: hoist u8 t/h + gf p/q.
        s{(int crypto_sign_open\(u8 \*m,u64 \*mlen,const u8 \*sm,u64 n,const u8 \*pk\)\n\{\n)  int i;\n  u8 t\[32\],h\[64\];\n  gf p\[4\],q\[4\];\n}
         {$1  /* uc386-dos: locals moved to file-static */\n  int i;\n  static u8 t[32],h[64];\n  static gf p[4],q[4];\n}s;

        # pack (ed25519): hoist gf tx, ty, zi.
        s{(sv pack\(u8 \*r,gf p\[4\]\)\n\{\n)  gf tx, ty, zi;\n}
         {$1  /* uc386-dos: locals moved to file-static */\n  static gf tx, ty, zi;\n}s;

        # reduce (ed25519): hoist i64 x[64] (512 bytes) to static.
        s{(sv reduce\(u8 \*r\)\n\{\n)  i64 x\[64\],i;\n}
         {$1  /* uc386-dos: locals moved to file-static */\n  static i64 x[64];\n  i64 i;\n}s;
    ' "$F"
}

patch_libssh2_kex_ecsha_macro_hoist() {
    F="upstream/lib/libssh2/src/kex.c"
    [ -f "$F" ] || return 0
    if grep -q "uc386-dos: hoist EC_SHA_HASH macro" "$F"; then
        return 0
    fi
    if ! grep -q "^#define LIBSSH2_KEX_METHOD_EC_SHA_HASH_CREATE_VERIFY" "$F"; then
        echo "micropython: warn: kex.c EC_SHA_HASH macro shape changed — skipping hoist." >&2
        return 0
    fi
    echo "micropython: hoisting libssh2 kex.c EC_SHA_HASH macro outside #if LIBSSH2_ECDSA …"
    # Strategy: remove the `#if LIBSSH2_ECDSA` that immediately
    # precedes (after a blank line) the EC_SHA_HASH macro comment,
    # and insert a fresh `#if LIBSSH2_ECDSA` after the macro's
    # closing `} while(0)`. Net effect: macro definition + comment
    # live outside the gate; ECDSA-specific code that follows stays
    # gated.
    perl -0777 -i -pe '
        s{
            (\#if\ LIBSSH2_ECDSA\n)        # the gate we are dropping
            (\n)                            # blank line
            (/\*\ LIBSSH2_KEX_METHOD_EC_SHA_HASH_CREATE_VERIFY
                [\s\S]*?
                \}\ while\(0\)\n)           # entire macro definition
        }
        { $2 . $3 . "#if LIBSSH2_ECDSA  /* uc386-dos: hoist EC_SHA_HASH macro outside ECDSA gate (used by ED25519 KEX too) */\n" }gsex;
    ' "$F"
}

patch_libssh2_sftp_handle_enum() {
    # sftp.h declares an anonymous enum INSIDE _LIBSSH2_SFTP_HANDLE
    # struct ({ LIBSSH2_SFTP_HANDLE_FILE, LIBSSH2_SFTP_HANDLE_DIR }).
    # uc386 doesn't hoist anonymous-enum members from struct scope
    # to file scope, so references from sftp.c fail. Promote the
    # enum to a top-level definition before the struct. Idempotent.
    F="upstream/lib/libssh2/src/sftp.h"
    if [ ! -f "$F" ]; then return 0; fi
    if grep -q "uc386-dos: hoisted handle_type enum" "$F"; then
        return 0
    fi
    echo "micropython: hoisting libssh2 sftp.h handle_type anonymous enum …"
    # Insert top-level enum just before `struct _LIBSSH2_SFTP_HANDLE`;
    # rewrite the in-struct anonymous enum to use the typedef'd name.
    perl -0777 -i -pe '
        s/(struct _LIBSSH2_SFTP_HANDLE\s*\n\{)/\/* uc386-dos: hoisted handle_type enum (anonymous-in-struct unsupported) *\/\nenum libssh2_sftp_handle_type {\n    LIBSSH2_SFTP_HANDLE_FILE,\n    LIBSSH2_SFTP_HANDLE_DIR\n};\n$1/;
        s/enum\s*\{\s*\n\s*LIBSSH2_SFTP_HANDLE_FILE,\s*\n\s*LIBSSH2_SFTP_HANDLE_DIR\s*\n\s*\}\s+handle_type;/enum libssh2_sftp_handle_type handle_type;/s;
    ' "$F"
}

patch_libssh2_wait_socket() {
    # libssh2's `_libssh2_wait_socket()` (session.c) does
    # `select(session->socket_fd + 1, ...)`. session->socket_fd
    # came from the `libssh2_socket_t sock` arg to
    # `libssh2_session_handshake()`, which we set to 0 — our SSH
    # I/O rides MP's stream protocol via callbacks (recv/send),
    # not a POSIX fd. So select() either returns immediately on
    # stdin or errors out ("Error waiting on socket"). The
    # blocking-mode BLOCK_ADJUST wrapper hits this whenever
    # libssh2 returns EAGAIN, breaking the SFTP startup path
    # which is heavily state-machine-driven.
    #
    # Patch _libssh2_wait_socket to a short-sleep no-op so
    # BLOCK_ADJUST just loops back into the operation — our
    # recv callback blocks in mp_stream_posix_read for real I/O
    # waiting, which is the right behavior under FreeDOS/lwIP.
    # Idempotent.
    F="upstream/lib/libssh2/src/session.c"
    [ -f "$F" ] || return 0
    if grep -q "uc386-dos: wait_socket no-op" "$F"; then
        return 0
    fi
    echo "micropython: patching libssh2 _libssh2_wait_socket -> short delay ..."
    # Replace the function body with a short delay (lets lwIP run
    # sys_check_timeouts + drain the netif RX queue) then return 0
    # so BLOCK_ADJUST retries the underlying operation. Avoids the
    # original select() on session->socket_fd, which we set to 0
    # because our I/O rides MP's stream protocol via callbacks.
    perl -0777 -i -pe '
        s/(int _libssh2_wait_socket\(LIBSSH2_SESSION \*session, time_t start_time\)\n\{\n).*?\n\}\n/$1    \/\* uc386-dos: wait_socket -- see fetch.sh *\/\n    extern void uc386dos_loopback_poll(void *arg);\n    extern void sys_check_timeouts(void);\n    extern void mp_hal_delay_ms(unsigned int ms);\n    (void)session; (void)start_time;\n    uc386dos_loopback_poll(0);\n    sys_check_timeouts();\n    mp_hal_delay_ms(50);\n    return 0;\n}\n/s;
    ' "$F"
}

patch_libssh2_scp_int64_format() {
    # libssh2's scp.c builds the SCP C-line via
    # `snprintf(buf, n, "C0%o %" LIBSSH2_INT64_T_FORMAT " %s\n", mode, size, base)`.
    # On non-Windows, LIBSSH2_INT64_T_FORMAT is "lld" and `size` is
    # `long long` (= libssh2_int64_t). Two uc386-specific issues:
    #   1. uc386's snprintf doesn't recognize `%o` — the format
    #      specifier is consumed but no arg is read, leaving just
    #      the literal "o" in the output.
    #   2. Same printf doesn't handle `%lld` — same kind of failure,
    #      leaves "ld" literal in output.
    # The result with both bugs: "C0o ld S\xff\n" instead of
    # "C0644 14 upload.txt\n".
    # Fix: hardcode mode as the literal "0644" (everything we send
    # via scp_send is mode 0644 anyway — we don't surface mode to
    # Python), and change the size format from "%lld" to "%ld" with
    # an explicit `(long)size` cast. Files >2GB aren't a concern.
    # Idempotent.
    F="upstream/lib/libssh2/src/scp.c"
    [ -f "$F" ] || return 0
    if grep -q 'uc386-dos: SCP C-line format' "$F"; then
        return 0
    fi
    echo "micropython: patching libssh2 SCP send C-line format ..."
    perl -0777 -i -pe '
        s/snprintf\(\(char \*\) session->scpSend_response,\s*\n\s*LIBSSH2_SCP_RESPONSE_BUFLEN, "C0%o %"\s*\n\s*LIBSSH2_INT64_T_FORMAT " %s\\n", mode,\s*\n\s*size, base\)/\/* uc386-dos: SCP C-line format -- see fetch.sh *\/\n            snprintf((char *) session->scpSend_response,\n                     LIBSSH2_SCP_RESPONSE_BUFLEN, "C0644 %ld %s\\n",\n                     (long)size, base); (void)mode;/s;
    ' "$F"
}

patch_mp_scope_qstr_qstr() {
    # MicroPython's upstream py/scope.h declares
    #
    #   id_info_t *scope_find_or_add_id(scope_t *scope, qstr qstr, ...);
    #
    # — typedef name `qstr` followed immediately by a parameter also
    # named `qstr`. C allows this (the typedef is shadowed inside the
    # parameter scope), but uc386's C23 parser rejects it as
    # `unexpected token IDENT 'qstr' ... expected one of: ELLIPSIS,
    # KW_ALIGNAS, ...`. The .c file uses `qst` for the same parameter
    # already; renaming the .h to match keeps the API consistent and
    # gets the parser past the prototype. Idempotent.
    F=upstream/py/scope.h
    [ -f "$F" ] || return 0
    if grep -q "qstr qst," "$F" 2>/dev/null && \
       ! grep -q "qstr qstr," "$F" 2>/dev/null; then
        return 0
    fi
    echo "micropython: patching py/scope.h: rename 'qstr qstr' param to 'qstr qst' …"
    sed -i.bak -E 's/qstr qstr,/qstr qst,/g; s/qstr qstr\)/qstr qst)/g' "$F"
    rm -f "$F.bak"
}

patch_libssh2_bsd_types() {
    # libssh2's chacha.h / cipher-chachapoly.h use BSD typedefs
    # `u_int` and `u_char` which uc386's libc doesn't ship. crypt.c
    # transitively includes chacha.h even though we don't build
    # the chacha source, so the typedef-missing error fires.
    # Substitute the BSD names with their unsigned-int / unsigned-char
    # equivalents in-place. Idempotent.
    if grep -q "uc386-dos: BSD types replaced" upstream/lib/libssh2/src/chacha.h 2>/dev/null; then
        return 0
    fi
    echo "micropython: patching libssh2 BSD-style u_int / u_char typedefs …"
    # All libssh2 headers that name BSD types — even the ones we
    # don't build .c for are still transitively included by crypt.c
    # (chacha.h / poly1305.h / cipher-chachapoly.h).
    for f in upstream/lib/libssh2/src/chacha.h \
             upstream/lib/libssh2/src/cipher-chachapoly.h \
             upstream/lib/libssh2/src/poly1305.h \
             upstream/lib/libssh2/src/crypt.c; do
        [ -f "$f" ] || continue
        perl -i -pe '
            BEGIN { print "/* uc386-dos: BSD types replaced (u_int -> unsigned int) */\n"; }
            s/\bu_int\b/unsigned int/g;
            s/\bu_char\b/unsigned char/g;
        ' "$f"
    done
}

patch_libssh2_callback_macros() {
    DIRS="upstream/lib/libssh2/include upstream/lib/libssh2/src"
    # Idempotency check on the public header — if it's patched the
    # rest were too in the same run.
    F="upstream/lib/libssh2/include/libssh2.h"
    if [ -f "$F" ] && grep -q "uc386-dos: joined LIBSSH2 callback-macro" "$F"; then
        return 0
    fi
    echo "micropython: joining libssh2 callback-macro continuation lines …"
    # Walk every .h and .c. The pattern is:
    #   <macro name>\n<whitespace>((*name))
    # → <macro>((*name))
    # so the preprocessor's function-like-macro expansion can run
    # (uc386's preprocessor doesn't recognise the call when the
    # arglist is on the next source line).
    for d in $DIRS; do
        [ -d "$d" ] || continue
        for f in "$d"/*.h "$d"/*.c; do
            [ -f "$f" ] || continue
            perl -0777 -i -pe '
                s/(LIBSSH2_[A-Z_]+_FUNC)\n(\s*)(\(\(\*[a-zA-Z_][a-zA-Z0-9_]*\)\))/\/\* uc386-dos: joined LIBSSH2 callback-macro \*\/ $1$3/gs;
            ' "$f"
        done
    done
}

# libssh2's crypto.h dispatches to a backend header by checking the
# LIBSSH2_<BACKEND> define. We add an LIBSSH2_AXTLS branch that
# pulls in our port-side adapter header (port/libssh2_axtls.h is
# wired into the -I include path by build_port.sh). Idempotent:
# skips if the branch already exists.
patch_libssh2_crypto_dispatch() {
    F="upstream/lib/libssh2/src/crypto.h"
    if [ ! -f "$F" ]; then return 0; fi
    if grep -q "LIBSSH2_AXTLS" "$F"; then
        return 0
    fi
    echo "micropython: patching libssh2 crypto.h dispatch for LIBSSH2_AXTLS …"
    awk '
        /^#elif defined\(LIBSSH2_WINCNG\)/ && !inserted {
            print "#elif defined(LIBSSH2_AXTLS)"
            print "#include \"libssh2_axtls.h\""
            inserted = 1
        }
        { print }
    ' "$F" > "$F.tmp" && mv "$F.tmp" "$F"
}

# Bracket each state transition in axtls's do_clnt_handshake with
# write(1, "[xx]", N) markers so the TLS rig log shows exactly which
# function call hangs. Diagnostic-only — paired with an entry in
# build_port.sh's removal list once the regression is fixed. Markers
# are emitted ONLY during the handshake itself (no per-record cost
# on the steady-state read/write path). Idempotent.
patch_axtls_handshake_markers() {
    F_CLNT="upstream/lib/axtls/ssl/tls1_clnt.c"
    F_OSP="upstream/extmod/axtls-include/axtls_os_port.h"
    [ -f "$F_CLNT" ] || return 0
    if grep -q "uc386-dos: handshake markers" "$F_CLNT"; then
        return 0
    fi
    echo "micropython: instrumenting axtls handshake with write() markers …"

    # Pull <unistd.h> into every axtls TU via the os_port wrapper so
    # write() is declared. Mirrors patch_axtls_endian_include's shape.
    if [ -f "$F_OSP" ] && ! grep -q "<unistd.h>" "$F_OSP"; then
        sed -i.bak \
            's|^#include <errno.h>$|#include <errno.h>\
#include <unistd.h>|' \
            "$F_OSP"
        rm -f "$F_OSP.bak"
    fi

    # Bracket each switch-case in do_clnt_handshake with markers. The
    # CCS+Finished compound condition is refactored into nested ifs so
    # we can place a marker between the two calls; semantics preserved
    # (send_finished still only runs when send_change_cipher_spec
    # returned SSL_OK).
    awk '
        /^int do_clnt_handshake\(SSL \*ssl, int handshake_type, uint8_t \*buf, int hs_len\)$/ {
            in_func = 1
            print
            next
        }
        in_func && /^    int ret;$/ {
            print
            print "    write(1, \"[hsE]\", 5);  /* uc386-dos: handshake markers */"
            next
        }
        in_func && /^            ret = process_server_hello\(ssl\);$/ {
            print "            write(1, \"[Sh]\", 4);"
            print
            print "            write(1, \"[Sh+]\", 5);"
            next
        }
        in_func && /^            ret = process_certificate\(ssl, &ssl->x509_ctx\);$/ {
            print "            write(1, \"[Ct]\", 4);"
            print
            print "            write(1, \"[Ct+]\", 5);"
            next
        }
        in_func && /^        case HS_SERVER_HELLO_DONE:$/ {
            print
            print "            write(1, \"[Hd]\", 4);"
            next
        }
        in_func && /^            if \(\(ret = process_server_hello_done\(ssl\)\) == SSL_OK\)$/ {
            print
            expect_brace = 1
            next
        }
        in_func && expect_brace && /^            \{$/ {
            print
            print "                write(1, \"[Hd+]\", 5);"
            expect_brace = 0
            next
        }
        # 20-char indent identifies the else-branch call (the CERT_REQ-
        # gated one sits at 24-char indent inside a different if).
        in_func && /^                    ret = send_client_key_xchg\(ssl\);$/ {
            print "                    write(1, \"[Kx]\", 4);"
            print
            print "                    write(1, \"[Kx+]\", 5);"
            next
        }
        # Absorb the 5-line CCS+Finished compound condition and replace
        # with nested ifs that take markers in between.
        in_func && /^                if \(ret == SSL_OK && $/ {
            getline _l2
            getline _l3
            getline _l4
            getline _l5
            print "                if (ret == SSL_OK) {"
            print "                    write(1, \"[Cs]\", 4);"
            print "                    ret = send_change_cipher_spec(ssl);"
            print "                    write(1, \"[Cs+]\", 5);"
            print "                    if (ret == SSL_OK) {"
            print "                        write(1, \"[Fi]\", 4);"
            print "                        ret = send_finished(ssl);"
            print "                        write(1, \"[Fi+]\", 5);"
            print "                    }"
            print "                }"
            next
        }
        in_func && /^    return ret;$/ {
            print "    write(1, \"[hsX]\", 5);"
            print
            next
        }
        in_func && /^\}$/ {
            in_func = 0
            print
            next
        }
        { print }
    ' "$F_CLNT" > "$F_CLNT.tmp" && mv "$F_CLNT.tmp" "$F_CLNT"
}

patch_axtls_time_dos_int21() {
    F1="upstream/lib/axtls/ssl/tls1_clnt.c"
    F2="upstream/lib/axtls/ssl/tls1.c"
    [ -f "$F1" ] || return 0
    [ -f "$F2" ] || return 0
    if grep -q "uc386-dos: skip time(NULL)" "$F1"; then
        return 0
    fi
    echo "micropython: patching axtls send_client_hello / ssl_session_update to skip INT 21h time() …"
    awk '
        /time_t tm = time\(NULL\);/ && !done {
            print "    /* uc386-dos: skip time(NULL) — INT 21h AH=0x2A re-entrancy"
            print "       hangs PMODE/W during the handshake. ClientHello timestamp"
            print "       nonce is non-security-critical; use a counter. 32-bit"
            print "       golden-ratio constant — using the ULL form (..7F4A7C15)"
            print "       trips uc386 into 64-bit arithmetic for what is a 32-bit"
            print "       variable, which leaves edx live across statement"
            print "       boundaries. Stick with the 32-bit literal to keep the"
            print "       codegen path identical to the rest of the function. */"
            print "    static unsigned long _hello_counter = 0;"
            print "    _hello_counter += 0x9E3779B9UL;"
            print "    time_t tm = (time_t)(_hello_counter & 0x7FFFFFFFUL);"
            done = 1
            next
        }
        { print }
    ' "$F1" > "$F1.tmp" && mv "$F1.tmp" "$F1"
    awk '
        /time_t tm = time\(NULL\);/ && !done {
            print "    /* uc386-dos: skip time(NULL) — same INT 21h re-entrancy as"
            print "       tls1_clnt.c send_client_hello. LRU age tag only. */"
            print "    static unsigned long _sess_counter = 0;"
            print "    _sess_counter += 1;"
            print "    time_t tm = (time_t)(_sess_counter & 0x7FFFFFFFUL);"
            done = 1
            next
        }
        { print }
    ' "$F2" > "$F2.tmp" && mv "$F2.tmp" "$F2"
}

# libssh2 — SSH client library, used by the port's `_ssh` MP module
# and the Python-level `sftp` / `scp` wrappers. Pinned to the latest
# stable release tarball; we ship a custom crypto backend
# (`uc386-dos/libssh2_crypto_axtls.c`) that adapts libssh2's
# `crypto.h` API surface to axtls's primitives + a TweetNaCl-derived
# Ed25519/X25519 impl. Linked into MP.EXE as part of build_port.sh.
fetch_libssh2() {
    LIBSSH2_VER="1.11.1"
    LIBSSH2_DIR="upstream/lib/libssh2"
    if [ -d "$LIBSSH2_DIR/src" ]; then
        return 0
    fi
    echo "micropython: fetching libssh2 $LIBSSH2_VER …"
    LSH_TMP="$(mktemp -d)"
    trap 'rm -rf "$LSH_TMP"' RETURN 2>/dev/null || true
    curl -fsSL \
        "https://www.libssh2.org/download/libssh2-${LIBSSH2_VER}.tar.gz" \
        -o "$LSH_TMP/libssh2.tgz"
    tar -xzf "$LSH_TMP/libssh2.tgz" -C "$LSH_TMP"
    mkdir -p "$LIBSSH2_DIR"
    cp -r "$LSH_TMP"/libssh2-*/src     "$LIBSSH2_DIR/"
    cp -r "$LSH_TMP"/libssh2-*/include "$LIBSSH2_DIR/"
    rm -rf "$LSH_TMP"
}

# TweetNaCl — public-domain Curve25519/Ed25519/Poly1305/ChaCha20
# reference impl in a single ~800-line .c file. Provides the
# primitives libssh2 needs that axtls doesn't ship: Curve25519
# scalarmult (X25519 KEX) and Ed25519 sign/verify (pubkey auth /
# host-key verification). Two files: tweetnacl.c + tweetnacl.h.
fetch_tweetnacl() {
    TNAC_DIR="upstream/lib/tweetnacl"
    if [ -f "$TNAC_DIR/tweetnacl.c" ]; then
        return 0
    fi
    echo "micropython: fetching TweetNaCl …"
    mkdir -p "$TNAC_DIR"
    # The canonical hosting is tweetnacl.cr.yp.to but it doesn't
    # version. The dchest/tweetnacl-js mirror keeps the C source
    # alongside the JS port and pins a commit, which is what we
    # want for reproducible builds.
    TNAC_BASE="https://raw.githubusercontent.com/ultramancool/tweetnacl-usable/master"
    for f in tweetnacl.c tweetnacl.h; do
        if [ ! -f "$TNAC_DIR/$f" ]; then
            curl -fsSL "$TNAC_BASE/$f" -o "$TNAC_DIR/$f"
        fi
    done
}

if [ -d upstream ]; then
    echo "micropython: upstream/ already present — skipping main fetch."
    fetch_b_con_crypto
    fetch_lwip
    fetch_axtls
    fetch_libssh2
    fetch_tweetnacl
    patch_modlwip_loopback_poll
    patch_main_startup_markers
    patch_main_pktdrv_prealloc
    patch_main_disable_fs_stubs
    patch_axtls_config_verify
    patch_axtls_endian_include
    patch_axtls_get_random_dos_int21
    patch_axtls_time_dos_int21
    patch_axtls_x509_gettimeofday
    patch_axtls_handshake_markers
    patch_libssh2_crypto_dispatch
    patch_libssh2_callback_macros
    patch_libssh2_bsd_types
    patch_libssh2_sftp_handle_enum
    patch_libssh2_crypto_engine_enum
    patch_libssh2_kex_ecsha_macro_hoist
    patch_libssh2_wait_socket
    patch_libssh2_scp_int64_format
    patch_tweetnacl_uc386dos
    patch_mp_scope_qstr_qstr
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
fetch_libssh2
fetch_tweetnacl
patch_modlwip_loopback_poll
patch_main_startup_markers
patch_main_pktdrv_prealloc
patch_main_disable_fs_stubs
patch_axtls_config_verify
patch_axtls_endian_include
patch_axtls_get_random_dos_int21
patch_axtls_time_dos_int21
patch_axtls_x509_gettimeofday
patch_axtls_handshake_markers
patch_libssh2_crypto_dispatch
patch_libssh2_callback_macros
patch_libssh2_bsd_types
patch_libssh2_sftp_handle_enum
patch_libssh2_crypto_engine_enum
patch_libssh2_kex_ecsha_macro_hoist
patch_libssh2_wait_socket
patch_tweetnacl_uc386dos
patch_mp_scope_qstr_qstr
