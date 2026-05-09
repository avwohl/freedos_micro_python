#!/bin/sh
# Per-file triage of the upstream/py/ core sources through uc386.
#
# This is NOT a full build (yet) — MicroPython needs a port shim
# (mp_hal_stdout_tx_*, mp_hal_delay_*, a fixed-region heap for GC,
# argv plumbing) before it can produce a runnable image. Today this
# script answers the prerequisite question: how many of the ~50
# platform-independent .c files in py/ compile cleanly through
# uc386 → NASM-ready .asm? An error histogram surfaces the top
# blockers as concrete tickets.
#
# Outputs:
#   build/<name>.asm   — for every PASS
#   build/<name>.err   — uc386 stderr for every FAIL
#   build/triage.txt   — per-source PASS/FAIL summary
#   build/errors.txt   — first-line error histogram (sorted by count)
set -eu


if [ ! -d upstream ]; then
    echo "micropython: run ./fetch.sh first." >&2
    exit 1
fi

# See build_port.sh header for the dual-invocation contract.
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
SRC_DIR="upstream/py"
PORT_DIR="upstream/ports/minimal"   # supplies mpconfigport.h + mphalport.h

mkdir -p build build/genhdr

# Stub the upstream-generated headers that py/*.c expects. A real
# build runs upstream/py/makeqstrdefs.py + makemoduledefs.py to
# emit these from the source tree; for triage we ship empty stubs
# so the preprocessor finds them and we see compile-class failures
# (uc386 limitations) instead of a wall of missing-header errors.
# Patch upstream/extmod/modtime.c so its `#include MICROPY_PY_TIME_INCLUDEFILE`
# becomes a literal include of our port shim. uc386's preprocessor
# doesn't support macro-name-in-#include (the GCC/Clang feature
# where the preprocessor expands the macro and then re-tokenizes).
# Idempotent: checks for the already-patched literal before
# re-applying.
if grep -q '^#include MICROPY_PY_TIME_INCLUDEFILE$' upstream/extmod/modtime.c; then
    # Bare filename — found via build_port.sh's `-I uc386-dos` search path.
    sed -i.bak \
        's|^#include MICROPY_PY_TIME_INCLUDEFILE$|#include "modtime_uc386dos.c"|' \
        upstream/extmod/modtime.c
    rm -f upstream/extmod/modtime.c.bak
fi

# Patch upstream/ports/minimal/main.c so its stub `mp_import_stat`
# and `mp_lexer_new_from_file` don't collide with the real
# implementations our port provides in `uc386-dos/file_uc386dos.c`.
# The minimal port hardcodes "no filesystem" responses; we overrride
# with INT 21h-backed file I/O via uc386's libc. Idempotent: checks
# for the already-patched marker before re-applying.
if grep -q "^mp_import_stat_t mp_import_stat" upstream/ports/minimal/main.c; then
    sed -i.bak \
        -e 's|^mp_lexer_t \*mp_lexer_new_from_file(qstr filename) {|static mp_lexer_t *_unused_mp_lexer_new_from_file(qstr filename) { (void)filename;|' \
        -e 's|^mp_import_stat_t mp_import_stat(const char \*path) {|static mp_import_stat_t _unused_mp_import_stat(const char *path) { (void)path;|' \
        upstream/ports/minimal/main.c
    rm -f upstream/ports/minimal/main.c.bak
fi

# Patch upstream/ports/minimal/main.c to initialize sys.argv as an
# empty list right after mp_init(). MICROPY_PY_SYS_ARGV expects the
# `mp_sys_argv_obj` root pointer to hold a real list object — without
# this init, sys.argv reads back as a half-initialized struct and
# crashes on `len(sys.argv)`. Idempotent: only inserts if not already
# present.
if ! grep -q "mp_obj_list_init.*mp_sys_argv_obj" upstream/ports/minimal/main.c; then
    sed -i.bak \
        's|^    mp_init();$|    mp_init();\n    mp_obj_list_init((mp_obj_list_t *)\&MP_STATE_VM(mp_sys_argv_obj), 0);|' \
        upstream/ports/minimal/main.c
    rm -f upstream/ports/minimal/main.c.bak
fi

# Patch upstream/ports/minimal/main.c to wire up `MICROPY_STACK_CHECK`.
# `mp_stack_ctrl_init()` captures the real stack top from a local
# stack variable; `mp_stack_set_limit(LIMIT)` sets the recursion-
# depth cap. Without these calls, every `mp_stack_check()` fires
# immediately (stack_top is NULL → mp_stack_usage returns a huge
# value → recursion-depth raise infinite-loops on its own setup).
# 0xC0000 = 768 KB, leaving 256 KB margin in dos_emu's 1 MB stack.
# Idempotent: only inserts if not already present.
if ! grep -q "mp_stack_ctrl_init" upstream/ports/minimal/main.c; then
    sed -i.bak \
        's|^    mp_init();$|    mp_stack_ctrl_init();\n    mp_stack_set_limit(0xC0000);\n    mp_init();|' \
        upstream/ports/minimal/main.c
    rm -f upstream/ports/minimal/main.c.bak
fi

# Patch upstream/py/formatfloat.c so the DOUBLE-mode `repr()` doesn't
# request more digits than uc386's double-precision FPU can deliver.
# Default is `MAX_MANTISSA_DIGITS=19` (designed for the EXACT formatter
# that runs on long double, ~80-bit). uc386 lowers double through the
# x87 FPU but stores `long double` as 8 bytes (= double), so the
# APPROX formatter at 19 digits surfaces the last 3 noise digits as
# wrong values: `print(4.0)` becomes `3.99999999999999382` instead of
# `4.0`. Lower the request to SAFE_MANTISSA_DIGITS=16 (already
# defined adjacent in the file). Idempotent: checks for the
# already-patched value before re-applying.
if grep -q "^#define MAX_MANTISSA_DIGITS  (19)$" upstream/py/formatfloat.c; then
    sed -i.bak 's/^#define MAX_MANTISSA_DIGITS  (19)$/#define MAX_MANTISSA_DIGITS  (16)  \/\/ uc386: long double == double, cap at SAFE/' upstream/py/formatfloat.c
    rm -f upstream/py/formatfloat.c.bak
fi

# Patch upstream/py/parsenum.c to neutralize the
# `assert(sizeof(mp_large_float_t) > sizeof(mp_float_t))` in
# `mp_decimal_exp`. uc386 stores long double as 8 bytes (= double),
# so the assert fires at runtime on the first compile-time-evaluated
# float. The algorithm still works — just at double precision —
# and the verify-retry loop in formatfloat.c delivers correctness
# regardless. Idempotent: leaves the assert commented after first
# patch.
if grep -q "^    assert(sizeof(mp_large_float_t) > sizeof(mp_float_t));$" upstream/py/parsenum.c; then
    sed -i.bak \
        's|^    assert(sizeof(mp_large_float_t) > sizeof(mp_float_t));$|    /* uc386: removed — long double == double, see mpconfigport.h */|' \
        upstream/py/parsenum.c
    rm -f upstream/py/parsenum.c.bak
fi

if [ ! -f build/genhdr/qstrdefs.generated.h ]; then
    # Emit a triage qstr table by grep over upstream/py/ +
    # upstream/shared/. Real builds use upstream's
    # tools/makeqstrdefs.py which preprocesses each TU to find
    # MP_QSTR_x macro uses; for triage we approximate with a grep
    # over the source tree, which over-includes (any string that
    # parses as an identifier becomes a qstr) but keeps the mapping
    # complete enough that the enum in py/qstr.h covers every
    # reference downstream code (py/, shared/runtime, etc.) makes.
    #
    # The first entry (MP_QSTRnull) goes in the static pool as
    # QDEF0; everything else goes in the main pool as QDEF1.
    # Reason: `mp_qstr_const_pool` (the main pool) has
    # `is_sorted = true` and the binary search in
    # `qstr_find_strn` does `pool->len - 1` as its upper bound —
    # which underflows to 0xFFFFFFFF when `pool->len == 0`,
    # then loops the binary search into out-of-bounds reads.
    # A real upstream build always has at least one QDEF1 entry,
    # so the empty-main-pool case never happens. We avoid it by
    # routing every grep'd qstr to QDEF1.
    {
        # MP_QSTRnull (id 0) is the null/sentinel qstr. The rest of
        # the static + unsorted qstrs (`__add__`, `print`, `__name__`,
        # ...) are emitted by gen_qstrdefs.py from upstream's
        # `static_qstr_list` + `unsorted_qstr_list` so they get
        # ids < 256 — required for the byte-stored
        # `mp_binary_op_method_name` table at py/objtype.c:483.
        echo "QDEF0(MP_QSTRnull, 0, 0, \"\")"
        # `MP_QSTR_<name>` — skip the 8-char `MP_QSTR_` prefix (M-P-_-Q-S-T-R-_)
        # to recover the actual qstr string. Earlier the prefix-strip used 7
        # chars and left a stray leading underscore (`__repl_print__` came
        # out as `___repl_print__`), making LOAD_NAME for builtins fail and
        # value-print compile crash inside mp_obj_equal_not_equal.
        #
        # Emit `length(name)` as the third QDEF1 field. qstr_find_strn's
        # post-binary-search linear sweep checks lengths[at] == str_len
        # before doing memcmp. Length 0 (the previous heuristic) made
        # every static qstr fail the length check, causing
        # `qstr_find_strn("__name__")` to return MP_QSTRnull even
        # though the qstr was in the pool — so the bytecode compiler
        # invented a fresh dynamic qstr for every identifier and
        # LOAD_NAME against the static-init dict_main key (whose qstr
        # id is the static 67) would never match.
        # gen_qstrdefs.py reverse-mangles each `MP_QSTR_<sanitized>`
        # macro back to its source string (e.g. `MP_QSTR__0x0a_` →
        # `"\n"`, `MP_QSTR__lt_stdin_gt_` → `"<stdin>"`) using
        # upstream's own codepoint2name table for fidelity, then emits
        # QDEF1 lines sorted by the *original* string in ASCII order.
        #
        # Sort key matters: qstr_find_strn does
        # `strncmp(probe, pool->qstrs[mid], n)` against the 4th QDEF1
        # field (the un-escaped string). The pool's `is_sorted=true`
        # invariant therefore requires sort-by-original. Earlier we
        # used `LC_ALL=C sort -u` over macro names, which coincided
        # for pure-identifier qstrs (`print`, `__name__`) but broke
        # for escaped ones — `MP_QSTR__0x0a_` lex-orders near `_`,
        # while its actual byte (0x0A) would sort first. With the
        # macro-name-as-payload heuristic, escaped qstrs also rendered
        # the *macro tail* as their string content, so `print()`'s
        # trailing `\n` showed up as the literal text `_0x0a_`.
        # `--bytes-hash 2` matches MICROPY_QSTR_BYTES_IN_HASH at
        # ROM_LEVEL_EXTRA_FEATURES (uc386-dos/mpconfigport.h sets
        # ROM_LEVEL=EXTRA, which makes BYTES_IN_HASH=2). Required:
        # qstr_find_strn's post-binary-search filter does
        # `pool->hashes[at] == str_hash` before memcmp at any
        # non-zero MICROPY_QSTR_BYTES_IN_HASH — a stale `--bytes-hash 1`
        # gen would emit 8-bit hashes and the runtime would
        # truncate the lookup hash to 16 bits, mismatching every
        # entry.
        #
        # The grep also pulls in X-macro NAMES from moderrno.c's
        # MICROPY_PY_ERRNO_LIST so the `MP_QSTR_##e` token paste
        # in moderrno.c's globals table resolves at compile time.
        # Without this, enabling MICROPY_PY_ERRNO would fail with
        # `__static_moderrno__errorcode_table.value: float init must
        # be a constant expression (got Identifier)` because uc386
        # can't resolve `MP_QSTR_EPERM` etc. as enum constants.
        # Translate `X(NAME)` lines into `MP_QSTR_NAME` lines that
        # gen_qstrdefs.py treats identically to a regular reference.
        {
            grep -rhoE "MP_QSTR_[A-Za-z_][A-Za-z0-9_]*" \
                    upstream/py/ upstream/shared/ upstream/extmod/ \
                    uc386-dos/
            # POSIX `[[:space:]]` rather than `\s` — macOS's BSD
            # `sed -E` doesn't honor PCRE shorthand in BRE/ERE.
            grep -hoE "^[[:space:]]*X\([A-Z][A-Z0-9_]*\)" \
                    upstream/py/moderrno.c \
                | sed -E 's/^[[:space:]]*X\(([A-Z][A-Z0-9_]*)\)/MP_QSTR_\1/'
        } | "$PYTHON" "${FREEDOS_MP_GEN_QSTRDEFS:-$(dirname "$0")/../gen_qstrdefs.py}" --bytes-hash 2
    } > build/genhdr/qstrdefs.generated.h
fi
# Always regenerate moduledefs.h — it's small and the cache turned
# stealth-stale when we added new UCDOS_MOD_ENTRY_* entries below
# without touching the file's mtime, so the new module didn't
# register and its `import` raised at runtime.
cat > build/genhdr/moduledefs.h <<'EOF'
// Hand-rolled equivalent of `upstream/py/makemoduledefs.py`'s output,
// covering the modules our uc386-dos port supports at the
// CORE_FEATURES ROM level. A real upstream build runs
// `tools/makeqstrdefs.py cat module` to preprocess each TU and
// extract MP_REGISTER_MODULE invocations after `#if` filtering, then
// pipes the result through makemoduledefs.py. We approximate by
// emitting each entry under the same `#if` gate the module's source
// file uses, so flipping `MICROPY_PY_<X>` in mpconfigport.h adds or
// drops the entry consistently.
//
// Modules NOT registered here:
//   - cmath         — requires MICROPY_PY_CMATH; we have float math
//                     via the x87 FPU but no complex-number support
//                     today.
//   - _thread       — requires MICROPY_PY_THREAD (single-threaded
//                     DOS, no need today).
//   - weakref       — requires MICROPY_PY_WEAKREF (off at CORE).
//   - io            — requires MICROPY_PY_IO + a VFS implementation;
//                     the port has no VFS today so `open()` is a
//                     no-op. Pre-set MICROPY_PY_IO=0 in
//                     mpconfigport.h to skip.

// All modules registered as regular (non-extensible). Extensible
// only matters with a VFS so users can override built-ins with .py
// files; the port has no VFS so the distinction is moot.

extern const struct _mp_obj_module_t mp_module_builtins;
extern const struct _mp_obj_module_t mp_module_sys;
extern const struct _mp_obj_module_t mp_module___main__;

#if MICROPY_PY_GC
extern const struct _mp_obj_module_t mp_module_gc;
#define UCDOS_MOD_ENTRY_GC { MP_ROM_QSTR(MP_QSTR_gc), MP_ROM_PTR(&mp_module_gc) },
#else
#define UCDOS_MOD_ENTRY_GC
#endif

#if MICROPY_PY_MATH
extern const struct _mp_obj_module_t mp_module_math;
#define UCDOS_MOD_ENTRY_MATH { MP_ROM_QSTR(MP_QSTR_math), MP_ROM_PTR(&mp_module_math) },
#else
#define UCDOS_MOD_ENTRY_MATH
#endif

#if MICROPY_PY_MICROPYTHON
extern const struct _mp_obj_module_t mp_module_micropython;
#define UCDOS_MOD_ENTRY_MICROPYTHON { MP_ROM_QSTR(MP_QSTR_micropython), MP_ROM_PTR(&mp_module_micropython) },
#else
#define UCDOS_MOD_ENTRY_MICROPYTHON
#endif

#if MICROPY_PY_ARRAY
extern const struct _mp_obj_module_t mp_module_array;
#define UCDOS_MOD_ENTRY_ARRAY { MP_ROM_QSTR(MP_QSTR_array), MP_ROM_PTR(&mp_module_array) },
#else
#define UCDOS_MOD_ENTRY_ARRAY
#endif

#if MICROPY_PY_COLLECTIONS
extern const struct _mp_obj_module_t mp_module_collections;
#define UCDOS_MOD_ENTRY_COLLECTIONS { MP_ROM_QSTR(MP_QSTR_collections), MP_ROM_PTR(&mp_module_collections) },
#else
#define UCDOS_MOD_ENTRY_COLLECTIONS
#endif

#if MICROPY_PY_ERRNO
extern const struct _mp_obj_module_t mp_module_errno;
#define UCDOS_MOD_ENTRY_ERRNO { MP_ROM_QSTR(MP_QSTR_errno), MP_ROM_PTR(&mp_module_errno) },
#else
#define UCDOS_MOD_ENTRY_ERRNO
#endif

#if MICROPY_PY_STRUCT
extern const struct _mp_obj_module_t mp_module_struct;
#define UCDOS_MOD_ENTRY_STRUCT { MP_ROM_QSTR(MP_QSTR_struct), MP_ROM_PTR(&mp_module_struct) },
#else
#define UCDOS_MOD_ENTRY_STRUCT
#endif

#if MICROPY_PY_TIME
extern const struct _mp_obj_module_t mp_module_time;
#define UCDOS_MOD_ENTRY_TIME { MP_ROM_QSTR(MP_QSTR_time), MP_ROM_PTR(&mp_module_time) },
#else
#define UCDOS_MOD_ENTRY_TIME
#endif

#if MICROPY_PY_UCTYPES
extern const struct _mp_obj_module_t mp_module_uctypes;
#define UCDOS_MOD_ENTRY_UCTYPES { MP_ROM_QSTR(MP_QSTR_uctypes), MP_ROM_PTR(&mp_module_uctypes) },
#else
#define UCDOS_MOD_ENTRY_UCTYPES
#endif

#if MICROPY_PY_RANDOM
extern const struct _mp_obj_module_t mp_module_random;
#define UCDOS_MOD_ENTRY_RANDOM { MP_ROM_QSTR(MP_QSTR_random), MP_ROM_PTR(&mp_module_random) },
#else
#define UCDOS_MOD_ENTRY_RANDOM
#endif

#if MICROPY_PY_BINASCII
extern const struct _mp_obj_module_t mp_module_binascii;
#define UCDOS_MOD_ENTRY_BINASCII { MP_ROM_QSTR(MP_QSTR_binascii), MP_ROM_PTR(&mp_module_binascii) },
#else
#define UCDOS_MOD_ENTRY_BINASCII
#endif

#if MICROPY_PY_HASHLIB
extern const struct _mp_obj_module_t mp_module_hashlib;
#define UCDOS_MOD_ENTRY_HASHLIB { MP_ROM_QSTR(MP_QSTR_hashlib), MP_ROM_PTR(&mp_module_hashlib) },
#else
#define UCDOS_MOD_ENTRY_HASHLIB
#endif

#if MICROPY_PY_RE
extern const struct _mp_obj_module_t mp_module_re;
#define UCDOS_MOD_ENTRY_RE { MP_ROM_QSTR(MP_QSTR_re), MP_ROM_PTR(&mp_module_re) },
#else
#define UCDOS_MOD_ENTRY_RE
#endif

#if MICROPY_PY_CMATH
extern const struct _mp_obj_module_t mp_module_cmath;
#define UCDOS_MOD_ENTRY_CMATH { MP_ROM_QSTR(MP_QSTR_cmath), MP_ROM_PTR(&mp_module_cmath) },
#else
#define UCDOS_MOD_ENTRY_CMATH
#endif

// `os` is the custom uc386-dos shim from `uc386-dos/os_uc386dos.c`
// (mkdir/rmdir/unlink/rename/chdir/getcwd/listdir backed by INT 21h
// via uc386's libc). Always registered — there's no MICROPY_PY_OS
// gate on our shim.
extern const struct _mp_obj_module_t mp_module_os;
#define UCDOS_MOD_ENTRY_OS { MP_ROM_QSTR(MP_QSTR_os), MP_ROM_PTR(&mp_module_os) },

#if MICROPY_PY_HEAPQ
extern const struct _mp_obj_module_t mp_module_heapq;
#define UCDOS_MOD_ENTRY_HEAPQ { MP_ROM_QSTR(MP_QSTR_heapq), MP_ROM_PTR(&mp_module_heapq) },
#else
#define UCDOS_MOD_ENTRY_HEAPQ
#endif

#if MICROPY_PY_DEFLATE
extern const struct _mp_obj_module_t mp_module_deflate;
#define UCDOS_MOD_ENTRY_DEFLATE { MP_ROM_QSTR(MP_QSTR_deflate), MP_ROM_PTR(&mp_module_deflate) },
#else
#define UCDOS_MOD_ENTRY_DEFLATE
#endif

#if MICROPY_PY_IO
extern const struct _mp_obj_module_t mp_module_io;
#define UCDOS_MOD_ENTRY_IO { MP_ROM_QSTR(MP_QSTR_io), MP_ROM_PTR(&mp_module_io) },
#else
#define UCDOS_MOD_ENTRY_IO
#endif

#if MICROPY_PY_JSON
extern const struct _mp_obj_module_t mp_module_json;
#define UCDOS_MOD_ENTRY_JSON { MP_ROM_QSTR(MP_QSTR_json), MP_ROM_PTR(&mp_module_json) },
#else
#define UCDOS_MOD_ENTRY_JSON
#endif

#if MICROPY_PY_PLATFORM
extern const struct _mp_obj_module_t mp_module_platform;
#define UCDOS_MOD_ENTRY_PLATFORM { MP_ROM_QSTR(MP_QSTR_platform), MP_ROM_PTR(&mp_module_platform) },
#else
#define UCDOS_MOD_ENTRY_PLATFORM
#endif

// `base64`, `shutil`, `tempfile` — port-supplied modules in
// uc386-dos/{base64,shutil,tempfile}_uc386dos.c. No upstream
// gates; always registered.
extern const struct _mp_obj_module_t mp_module_base64;
extern const struct _mp_obj_module_t mp_module_shutil;
extern const struct _mp_obj_module_t mp_module_tempfile;
#define UCDOS_MOD_ENTRY_BASE64   { MP_ROM_QSTR(MP_QSTR_base64),   MP_ROM_PTR(&mp_module_base64) },
#define UCDOS_MOD_ENTRY_SHUTIL   { MP_ROM_QSTR(MP_QSTR_shutil),   MP_ROM_PTR(&mp_module_shutil) },
#define UCDOS_MOD_ENTRY_TEMPFILE { MP_ROM_QSTR(MP_QSTR_tempfile), MP_ROM_PTR(&mp_module_tempfile) },

// `lwip` + `socket` — both back the same `mp_module_lwip` from
// upstream's extmod/modlwip.c. The Python-level `socket` API uses
// the lwIP raw API via that module.
#if MICROPY_PY_LWIP
extern const struct _mp_obj_module_t mp_module_lwip;
#define UCDOS_MOD_ENTRY_LWIP   { MP_ROM_QSTR(MP_QSTR_lwip),   MP_ROM_PTR(&mp_module_lwip) },
#define UCDOS_MOD_ENTRY_SOCKET { MP_ROM_QSTR(MP_QSTR_socket), MP_ROM_PTR(&mp_module_lwip) },
#else
#define UCDOS_MOD_ENTRY_LWIP
#define UCDOS_MOD_ENTRY_SOCKET
#endif

// `urllib` + `urllib_parse` — port-supplied. Registered as a
// `urllib` package shim (with `parse` as an attribute, so
// `from urllib import parse` works) and a top-level `urllib_parse`
// (so `import urllib_parse` works). `import urllib.parse` resolves
// via MP's dotted-import path: it imports `urllib`, then reads the
// `parse` attribute. We don't register a dotted name in the
// builtin-modules table — the qstr-grep doesn't see the dotted
// form anyway since `.` can't appear in a C identifier.
extern const struct _mp_obj_module_t mp_module_urllib;
extern const struct _mp_obj_module_t mp_module_urllib_parse;
#define UCDOS_MOD_ENTRY_URLLIB        { MP_ROM_QSTR(MP_QSTR_urllib),       MP_ROM_PTR(&mp_module_urllib) },
#define UCDOS_MOD_ENTRY_URLLIB_PARSE  { MP_ROM_QSTR(MP_QSTR_urllib_parse), MP_ROM_PTR(&mp_module_urllib_parse) },

// `uc386_net` — port-supplied. Control surface for the lwIP eth
// netif sitting on the INT 0x83 packet-driver shim. Always
// registered when LWIP is on (no separate config gate yet).
#if MICROPY_PY_LWIP
extern const struct _mp_obj_module_t mp_module_uc386_net;
#define UCDOS_MOD_ENTRY_UC386_NET { MP_ROM_QSTR(MP_QSTR_uc386_net), MP_ROM_PTR(&mp_module_uc386_net) },
#else
#define UCDOS_MOD_ENTRY_UC386_NET
#endif

// `select` — extmod/modselect.c. Provides `select.select()` and
// `select.poll()` over the stream-protocol's poll hook (which
// modlwip.c's socket type implements). Useful for non-blocking
// I/O multiplexing.
#if MICROPY_PY_SELECT
extern const struct _mp_obj_module_t mp_module_select;
#define UCDOS_MOD_ENTRY_SELECT { MP_ROM_QSTR(MP_QSTR_select), MP_ROM_PTR(&mp_module_select) },
#else
#define UCDOS_MOD_ENTRY_SELECT
#endif

// `tls` (and the `ssl` alias) — extmod/modtls_axtls.c. Wraps a
// socket through axtls: SSLContext + wrap_socket -> SSLSocket
// implementing the standard stream protocol. The actual axtls
// library lives at upstream/lib/axtls/{ssl,crypto}/.
#if MICROPY_PY_SSL
extern const struct _mp_obj_module_t mp_module_tls;
#define UCDOS_MOD_ENTRY_TLS { MP_ROM_QSTR(MP_QSTR_tls), MP_ROM_PTR(&mp_module_tls) },
// Python's stdlib name is `ssl`; map it to the same object so user
// code written against CPython continues to work.
#define UCDOS_MOD_ENTRY_SSL { MP_ROM_QSTR(MP_QSTR_ssl), MP_ROM_PTR(&mp_module_tls) },
#else
#define UCDOS_MOD_ENTRY_TLS
#define UCDOS_MOD_ENTRY_SSL
#endif

#define MICROPY_REGISTERED_MODULES \
    { MP_ROM_QSTR(MP_QSTR_builtins), MP_ROM_PTR(&mp_module_builtins) }, \
    { MP_ROM_QSTR(MP_QSTR_sys), MP_ROM_PTR(&mp_module_sys) }, \
    { MP_ROM_QSTR(MP_QSTR___main__), MP_ROM_PTR(&mp_module___main__) }, \
    UCDOS_MOD_ENTRY_GC \
    UCDOS_MOD_ENTRY_MATH \
    UCDOS_MOD_ENTRY_MICROPYTHON \
    UCDOS_MOD_ENTRY_ARRAY \
    UCDOS_MOD_ENTRY_COLLECTIONS \
    UCDOS_MOD_ENTRY_ERRNO \
    UCDOS_MOD_ENTRY_STRUCT \
    UCDOS_MOD_ENTRY_TIME \
    UCDOS_MOD_ENTRY_UCTYPES \
    UCDOS_MOD_ENTRY_RANDOM \
    UCDOS_MOD_ENTRY_BINASCII \
    UCDOS_MOD_ENTRY_HASHLIB \
    UCDOS_MOD_ENTRY_RE \
    UCDOS_MOD_ENTRY_CMATH \
    UCDOS_MOD_ENTRY_OS \
    UCDOS_MOD_ENTRY_HEAPQ \
    UCDOS_MOD_ENTRY_DEFLATE \
    UCDOS_MOD_ENTRY_IO \
    UCDOS_MOD_ENTRY_JSON \
    UCDOS_MOD_ENTRY_PLATFORM \
    UCDOS_MOD_ENTRY_BASE64 \
    UCDOS_MOD_ENTRY_SHUTIL \
    UCDOS_MOD_ENTRY_TEMPFILE \
    UCDOS_MOD_ENTRY_URLLIB \
    UCDOS_MOD_ENTRY_URLLIB_PARSE \
    UCDOS_MOD_ENTRY_LWIP \
    UCDOS_MOD_ENTRY_SOCKET \
    UCDOS_MOD_ENTRY_UC386_NET \
    UCDOS_MOD_ENTRY_SELECT \
    UCDOS_MOD_ENTRY_TLS \
    UCDOS_MOD_ENTRY_SSL

// Module attribute-access delegation table — modules whose attr
// loads/stores need to dispatch through a port-supplied function.
// Picks up `MP_REGISTER_MODULE_DELEGATION(mod, fun)` calls.
// Currently only `sys` registers one (modsys.c:412): the
// `mp_module_sys_attr` function which dispatches `sys.path` /
// `sys.ps1` / `sys.ps2` / `sys.tracebacklimit` reads and writes
// against `MP_STATE_VM(sys_mutable[])`.
#if MICROPY_PY_SYS_ATTR_DELEGATION
extern void mp_module_sys_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest);
#define MICROPY_MODULE_DELEGATIONS \
    { MP_ROM_PTR(&mp_module_sys), mp_module_sys_attr },
#endif

#define MICROPY_REGISTERED_EXTENSIBLE_MODULES
EOF
[ -f build/genhdr/mpversion.h ] || cat > build/genhdr/mpversion.h <<'EOF'
// Triage stub.
#define MICROPY_GIT_TAG "uc386-triage"
#define MICROPY_GIT_HASH "0000000"
#define MICROPY_BUILD_DATE "2026-05-01"
EOF
if [ ! -f build/genhdr/root_pointers.h ]; then
    # Real builds run upstream/py/makeqstrdefs.py with mode=root_pointer
    # to scan all C sources for `MP_REGISTER_ROOT_POINTER(<decl>);`
    # declarations and emit them as struct fields of `_mp_state_vm_t`
    # (via py/mpstate.h's `#include "genhdr/root_pointers.h"`). For
    # triage we approximate with grep — the macro pattern is regular,
    # we just take everything between the parens and emit it as a
    # struct member terminated with a semicolon.
    {
        echo "// Triage stub. Real build emits MP_REGISTER_ROOT_POINTER entries here."
        grep -rhE "^MP_REGISTER_ROOT_POINTER\(.*\);" \
                upstream/py/ upstream/shared/ upstream/extmod/ \
            | sed -E 's#^MP_REGISTER_ROOT_POINTER\((.*)\);#    \1;#' \
            | sort -u
    } > build/genhdr/root_pointers.h
fi

# Triage stub: a one-line main() so uc386 (which requires a main
# function in every translation unit it compiles) accepts library
# .c files. We append it to each src on the fly. This answers
# "would this file compile if it were linked into a real port?"
# without the multi-file plumbing a real port needs.
TRIAGE_MAIN="build/_triage_main.c"
cat > "$TRIAGE_MAIN" <<'EOF'
// Synthetic main so uc386 has an entry-point during per-file triage.
// Real ports/<port>/main.c supplies its own main + mp_init/mp_deinit.
int main(int argc, char **argv) { (void)argc; (void)argv; return 0; }
EOF

TRIAGE="build/triage.txt"
ERR_HIST="build/errors.txt"
: > "$TRIAGE"
: > "$ERR_HIST"

PASS=0
FAIL=0
TOTAL=0

# Section accounting so the per-section pass/fail is visible.
PY_PASS=0; PY_FAIL=0; PY_TOTAL=0
SH_PASS=0; SH_FAIL=0; SH_TOTAL=0

triage_one() {
    src="$1"
    section="$2"        # used only for the section-count update
    name_prefix="$3"    # disambiguates basenames between sections
    [ -f "$src" ] || return 0
    TOTAL=$((TOTAL + 1))
    name="${name_prefix}$(basename "$src" .c)"
    if "$PYTHON" -m uc386.main "$TRIAGE_MAIN" "$src" \
            -o "build/${name}.asm" \
            -I "$INCLUDE" \
            -I "upstream" \
            -I "$PORT_DIR" \
            -I "build" \
            > "build/${name}.out" 2> "build/${name}.err"; then
        PASS=$((PASS + 1))
        if [ "$section" = py ]; then PY_PASS=$((PY_PASS + 1)); PY_TOTAL=$((PY_TOTAL + 1)); fi
        if [ "$section" = sh ]; then SH_PASS=$((SH_PASS + 1)); SH_TOTAL=$((SH_TOTAL + 1)); fi
        echo "$name: OK" >> "$TRIAGE"
        rm -f "build/${name}.err" "build/${name}.out"
    else
        FAIL=$((FAIL + 1))
        if [ "$section" = py ]; then PY_FAIL=$((PY_FAIL + 1)); PY_TOTAL=$((PY_TOTAL + 1)); fi
        if [ "$section" = sh ]; then SH_FAIL=$((SH_FAIL + 1)); SH_TOTAL=$((SH_TOTAL + 1)); fi
        first_line="$(head -1 "build/${name}.err" 2>/dev/null || echo unknown)"
        echo "$name: FAIL  $first_line" >> "$TRIAGE"
        # Strip filename + line numbers from the leading error so
        # the histogram clusters by error class.
        echo "$first_line" \
            | sed -E 's#^[^:]+:[0-9]+:[0-9]+:?##; s#^[^:]+:[0-9]+:?##' \
            | sed -E 's#  +# #g; s#^ +##' \
            >> "$ERR_HIST"
    fi
}

# py/ — the platform-independent core (132 sources today).
for src in "$SRC_DIR"/*.c; do
    triage_one "$src" py ""
done

# shared/{libc,readline,runtime,timeutils,netutils}/ — extra sources
# real ports pull in alongside py/. The minimal port uses the first
# three; richer ports (esp32, rp2, etc.) also pull in timeutils +
# netutils. Keeping them in the same triage answers "how close is a
# full port to compiling cleanly" not just "how clean is py/".
for shared_src in \
        upstream/shared/libc/printf.c \
        upstream/shared/libc/string0.c \
        upstream/shared/libc/__errno.c \
        upstream/shared/libc/abort_.c \
        upstream/shared/readline/readline.c \
        upstream/shared/runtime/pyexec.c \
        upstream/shared/runtime/stdout_helpers.c \
        upstream/shared/runtime/interrupt_char.c \
        upstream/shared/runtime/sys_stdio_mphal.c \
        upstream/shared/timeutils/timeutils.c \
        upstream/shared/netutils/netutils.c \
        upstream/shared/netutils/trace.c \
        upstream/shared/netutils/dhcpserver.c; do
    [ -f "$shared_src" ] || continue
    # name_prefix=shared_ so e.g. shared/libc/printf.c doesn't collide
    # with py/ — there is no collision today, but the prefix keeps
    # the name space clean and makes the section visible in triage.txt.
    triage_one "$shared_src" sh "shared_"
done

echo
echo "== triage: $PASS pass / $FAIL fail / $TOTAL total =="
echo "    py/                                          $PY_PASS / $PY_TOTAL"
echo "    shared/{libc,readline,runtime,timeutils,netutils}/  $SH_PASS / $SH_TOTAL"
echo
echo "Top error classes (count × class):"
sort "$ERR_HIST" | uniq -c | sort -rn | head -15
