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

if [ -d upstream ]; then
    echo "micropython: upstream/ already present — skipping main fetch."
    fetch_b_con_crypto
    fetch_lwip
    fetch_axtls
    patch_modlwip_loopback_poll
    patch_main_startup_markers
    patch_axtls_config_verify
    patch_axtls_endian_include
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
patch_axtls_config_verify
patch_axtls_endian_include
