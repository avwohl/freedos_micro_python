// uc386-dos MicroPython port config — ROM_LEVEL = EXTRA_FEATURES.
//
// Started life as a clone of `upstream/ports/minimal/mpconfigport.h`
// (MINIMUM ROM level + REPL + GC + external import). Walked up the
// ROM ladder slice by slice until we hit EXTRA_FEATURES wholesale,
// fixing each new failure as it surfaced (qstr-hash field width,
// X-macro qstrs for errno, IEEE-754 inf/nan via __builtin_*,
// static-pool placement for special-method dunders, ...). See
// `addons/gnu/micropython/NOTES.md` for the per-slice diary.
//
// Port-incompatible defaults explicitly turned off below.

#include <stdint.h>

// Use the setjmp/longjmp-backed NLR (non-local return) machinery.
// The default x86 NLR (`upstream/py/nlrx86.c`) is GCC inline asm,
// which uc_core / uc386 doesn't compile — `nlr_push`/`nlr_jump`
// would silently expand to no-ops, leaving uninitialized garbage in
// the nlr_buf and crashing the parser at the first exception. The
// setjmp path goes through uc386's libc `_setjmp`/`_longjmp`
// (lib/i386_dos_libc.asm), which is real i386 asm (saves 6 dwords:
// ebx/esi/edi/ebp/esp/eip). Pair this with the fix in
// `lib/include/setjmp.h` that widens jmp_buf to 24 bytes — the old
// 6-byte declaration would have caused setjmp to buffer-overflow.
#define MICROPY_NLR_SETJMP                (1)

// EXTRA_FEATURES baseline. Lights up `compile()` / `eval()` /
// `exec()`, `input()`, `memoryview`, `MICROPY_PY_ALL_SPECIAL_METHODS`
// (`__add__` / `__radd__` / `__iadd__` / `__and__` / etc on class
// instances), `collections.deque`, math constants (pi/e/tau/inf/
// nan), `math.factorial`, `math.isclose`, `bytes.hex` /
// `bytes.fromhex`, `str.center` / `partition` / `splitlines`,
// `frozenset`, **f-strings**, function attribute access
// (`f.__name__`), `delattr`/`setattr`, REPL Emacs keys + auto-
// indent + Ctrl-C → KeyboardInterrupt, deque iter+subscr,
// bytearray slice-assign, plus dozens of smaller surface knobs.
#define MICROPY_CONFIG_ROM_LEVEL          (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)

// All previously-disabled EXTRA-default flags now enabled below
// (STACK_CHECK / PY_UCTYPES / TIME_TIME_TIME_NS / BUILTINS_HELP /
// MODULE___FILE__).

// Stack-overflow guard. With a 1 MB stack (dos_emu maps STACK_BASE
// 0x01000000..0x01100000), an unbounded recursion or huge frame
// would otherwise hit unmapped memory and surface as
// UC_ERR_WRITE_UNMAPPED instead of a clean RuntimeError. main.c
// (sed-patched in build.sh) calls `mp_stack_ctrl_init()` after
// mp_init to capture the real stack top, then
// `mp_stack_set_limit(0xC0000)` to give a 256 KB safety margin
// (i.e., raise RecursionError when usage hits 768 KB, well below
// the 1 MB hard limit).
#define MICROPY_STACK_CHECK               (1)

// `help()` builtin — uses the upstream default help text from
// `py/builtinhelp.c:mp_help_default_text`. With
// MICROPY_PY_BUILTINS_HELP_TEXT undefined, the default
// (`mp_help_default_text`) is used.
#define MICROPY_PY_BUILTINS_HELP          (1)

// `__file__` attribute on imported modules. Auto-set by
// upstream's `do_load_from_lexer` (builtinimport.c:158) using
// the lexer's `source_name` qstr — which our
// `uc386-dos/file_uc386dos.c:mp_lexer_new_from_file` initializes
// from the import filename. No port-supplied helper needed.
#define MICROPY_MODULE___FILE__           (1)
// `uctypes` — binary struct access for memoryview/buffer-like
// objects (define a layout dict, pin it onto a buffer, read/write
// fields by name). build_port.sh adds extmod/moductypes.c to the
// source list. The module gets registered under MP_QSTR_uctypes
// via MP_REGISTER_EXTENSIBLE_MODULE.
#define MICROPY_PY_UCTYPES                (1)

// `time.time()` / `time.localtime()` / `time.gmtime()` /
// `time.mktime()` — wired to the DOS RTC via INT 21h AH=0x2A
// (date) + AH=0x2C (time-of-day) in
// lib/i386_dos_libc.asm:_dos_get_datetime, then converted to
// seconds-since-epoch by upstream's shared/timeutils. The port
// shim lives in `uc386-dos/modtime_uc386dos.c` and is
// `#include`'d into extmod/modtime.c via the
// MICROPY_PY_TIME_INCLUDEFILE hook (provides
// `mp_time_time_get` + `mp_time_localtime_get`).
//
// MICROPY_TIMESTAMP_IMPL = 1 (UINT) forces `mp_timestamp_t` to
// be `mp_uint_t` (32-bit on i386). The default for
// MICROPY_EPOCH_IS_2000 is LONG_LONG, which we can't safely
// return from `time.time()` without longlong int support
// (`mp_obj_new_int_from_ll` is a stub in LONGINT_IMPL_NONE that
// always raises OverflowError). 32-bit unsigned seconds-since-
// 2000 covers through year 2136 — adequate for a DOS port.
//
// Caveat: TIME_TIME_NS gates BOTH `time.time()` AND
// `time.time_ns()` in upstream modtime.c. Calling
// `time.time_ns()` will raise `OverflowError("small int
// overflow")` because it routes through
// `mp_obj_new_int_from_ull(mp_hal_time_ns())` and the ull stub
// rejects everything. Lighting it up cleanly requires
// MICROPY_LONGINT_IMPL_LONGLONG.
#define MICROPY_TIMESTAMP_IMPL            (1)
#define MICROPY_PY_TIME_TIME_TIME_NS      (1)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (1)
#define MICROPY_PY_TIME_INCLUDEFILE       "uc386-dos/modtime_uc386dos.c"

// `open()` + `import xxx` (loading `xxx.py` from disk) wired through
// uc386's libc INT 21h file syscalls. We provide port-supplied
// `mp_builtin_open_obj` / `mp_import_stat` / `mp_lexer_new_from_file`
// in `uc386-dos/file_uc386dos.c` (no full VFS — just enough for
// flat .py imports and read/write file objects). MICROPY_PY_IO=1
// pulls in modio.c (io.IOBase, BytesIO, StringIO) and exposes
// `open` in the builtins table.
#define MICROPY_PY_IO                     (1)

// `sys`-module sub-features.
//   - sys.exit: raises `SystemExit`; pyexec_friendly_repl catches
//     it and bails out of the REPL loop, then main() returns to
//     the libc cleanup → INT 21h AH=4Ch with the exit code.
//   - sys.modules: dict of imported modules. `mp_init` already
//     initializes `mp_loaded_modules_dict` via
//     `mp_obj_dict_init(...)`; just exposing it as `sys.modules`.
//   - sys.argv: empty list. `mp_init`'s
//     `MICROPY_PY_SYS_PATH_ARGV_DEFAULTS` block (runtime.c:142)
//     auto-initializes both sys.path and sys.argv when the flags
//     are on, so no port shim needed.
//   - sys.path: list of directories to search on import. With
//     MICROPY_PY_SYS_PATH=1, builtinimport.c's `stat_top_level`
//     iterates the list and our `mp_import_stat` (file_uc386dos.c)
//     stat-checks each candidate. Initialized in mp_init with
//     a single empty-string entry meaning "current directory".
//     Enabling MICROPY_PY_SYS_PATH automatically enables
//     MICROPY_PY_SYS_ATTR_DELEGATION (per modsys.c:267), which
//     in turn requires MICROPY_MODULE_ATTR_DELEGATION (already
//     on at our EXTRA_FEATURES ROM level per mpconfig.h:1127).
#define MICROPY_PY_SYS_MODULES            (1)
#define MICROPY_PY_SYS_EXIT               (1)
#define MICROPY_PY_SYS_PATH               (1)
#define MICROPY_PY_SYS_ARGV               (1)

// Polish: introspection knobs default to off (or EVERYTHING-only) at
// EXTRA_FEATURES — opt in explicitly. All live in modules we already
// compile (modsys.c / objrange.c) so no new sources needed.
//
// `sys.exc_info()` returns the (type, value, tb) tuple for the
// currently-handled exception inside an `except:` block. Useful for
// generic logging / propagation.
#define MICROPY_PY_SYS_EXC_INFO           (1)
// `sys.tracebacklimit` lets user code cap traceback depth. Useful
// in DOS where stdout is precious screen real-estate.
#define MICROPY_PY_SYS_TRACEBACKLIMIT     (1)
// `range(3) == range(3)` returns True (without this, ranges only
// compare equal by identity). Tiny, but a frequent CPython surprise.
#define MICROPY_PY_BUILTINS_RANGE_BINOP   (1)

// errno requires:
//  - build.sh to pre-emit the EPERM/ENOENT/... qstrs (the module's
//    globals table uses `MP_QSTR_##e` token paste over its X-macro
//    list, which our grep-based gen_qstrdefs.py can't see otherwise).
//  - MICROPY_USE_INTERNAL_ERRNO=1 so MP_EPERM resolves to the
//    upstream's hardcoded MP_##e values rather than to the system's
//    EPERM macros from <errno.h>. uc386's libc errno.h ships only
//    a Linux subset (no EOPNOTSUPP / EADDRINUSE / ECONN* / EHOST* /
//    EALREADY / EINPROGRESS), so the system path leaves several
//    MP_##e references as bare Identifiers that fail const-eval.
#define MICROPY_USE_INTERNAL_ERRNO        (1)

// Float — uc386 lowers `double` through the x87 FPU and uc386's
// libc (lib/i386_dos_libc.asm) ships sin/cos/tan/asin/acos/atan/
// atan2/exp/log/log10/log2/expm1/pow/sqrt/floor/ceil/trunc/fmod/
// modf/fabs/copysign/signbit/isnan/isinf/isfinite/nan/nearbyint/
// ldexp/frexp/sinh/cosh/tanh/asinh/acosh/atanh/erf/erfc plus NaN
// stubs for tgamma/lgamma in raw 387 asm. DOUBLE rather than
// FLOAT so the libc's `sin`/`cos`/... unsuffixed names match what
// micropython calls (FLOAT_IMPL_FLOAT calls `sinf` / `cosf` /
// ... which we don't have).
#define MICROPY_FLOAT_IMPL                (MICROPY_FLOAT_IMPL_DOUBLE)
#define MICROPY_PY_MATH_SPECIAL_FUNCTIONS (1)

// Math edge-case fixes — defaults are 0 across all ROM levels
// because they cost a few extra branches. We're not size-constrained
// (the EXACT float formatter alone costs more), and matching CPython
// behavior at infinity / NaN boundaries makes test code that relies
// on `math.atan2(0, 0) == 0`-style assumptions actually portable.
// See py/modmath.c for the wrappers each flag enables.
#define MICROPY_PY_MATH_ATAN2_FIX_INFNAN  (1)
#define MICROPY_PY_MATH_FMOD_FIX_INFNAN   (1)
#define MICROPY_PY_MATH_MODF_FIX_NEGZERO  (1)
#define MICROPY_PY_MATH_POW_FIX_NAN       (1)
#define MICROPY_PY_MATH_GAMMA_FIX_NEGINF  (1)

// Float formatter — opt into the EXACT formatter (vs the default
// APPROX picked when long double has the same width as double).
// EXACT's headline value is a parse-and-retry loop in
// `py/formatfloat.c:472`: format with N digits, parse the result
// back, compare against the original; if not equal, retry with
// N+1. With APPROX the loop is gated out and the formatter walks
// to MAX_MANTISSA_DIGITS digits unconditionally — accumulating
// floating-point noise in each multiplication step and producing
// `print(4.0)` → `3.999999999999996` and `print(1e10)` →
// `09999999999.99998`. EXACT picks the SHORTEST decimal that
// round-trips, so simple values like 4.0 / 0.1 / 1e10 get the
// expected literal form back. Caveat: `mp_decimal_exp` (parsenum.c)
// has an `assert(sizeof(mp_large_float_t) > sizeof(mp_float_t))`
// guarding against picking EXACT on a platform where long double
// isn't wider than double — which IS our case (uc386 lowers long
// double through the x87 FPU but stores it as 8 bytes). build.sh
// sed-patches that assertion out: the algorithm still works,
// just at the same precision as APPROX would have used; the
// verify-retry loop is what actually delivers the correctness.
#define MICROPY_FLOAT_FORMAT_IMPL         (MICROPY_FLOAT_FORMAT_IMPL_EXACT)

// Long-long int support — heap-allocated `mp_obj_int_t` for
// values that don't fit a small int. Default is
// `LONGINT_IMPL_NONE`, where `mp_obj_new_int_from_ll` /
// `mp_obj_new_int_from_ull` are stubs that always raise
// `OverflowError("small int overflow")`. That makes
// `time.time_ns()`, large `struct.unpack` results, and other
// 64-bit-int paths unusable. LONGLONG enables the real
// implementations in `upstream/py/objint_longlong.c` (already
// in our source list) and pulls heap-allocated `mp_obj_int_t`
// instances onto the GC heap. Same surface as the upstream
// stm32 / esp32 ports.
#define MICROPY_LONGINT_IMPL              (MICROPY_LONGINT_IMPL_LONGLONG)

// `time` module — `time.ticks_ms`, `time.sleep_ms`, etc. wired
// through INT 1Ah AH=0 BIOS tick counter (~18.2 Hz, ~55 ms/tick)
// in lib/i386_dos_libc.asm and uc386-dos/mphal_uc386dos.c.
// Default-off at our ROM_LEVEL because upstream's ROM-level
// numbering puts BASIC_FEATURES (20) ABOVE CORE_FEATURES (10),
// so `MICROPY_PY_TIME = AT_LEAST_BASIC_FEATURES` evaluates to 0.
#define MICROPY_PY_TIME                   (1)

// extmod modules: `random`, `binascii`, `hashlib` (SHA-256 only),
// `re`. Each is a single source file added to build_port.sh's
// list and registered in build.sh's moduledefs.h. random's
// seed-init defaults to the BIOS tick counter so `random.seed()`
// with no arg picks up some entropy. hashlib pulls in the SHA-256
// implementation from `lib/crypto-algorithms/sha256.c` via inline
// `#include` (no separate source-list entry needed). re uses
// `lib/re1.5/*.c` similarly. binascii's CRC32 path is gated on
// `MICROPY_PY_DEFLATE` (off) so the unzip dep doesn't pull in.
#define MICROPY_PY_RANDOM                 (1)
#define MICROPY_PY_RANDOM_EXTRA_FUNCS     (1)
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC  (bios_ticks())
extern unsigned long bios_ticks(void);
#define MICROPY_PY_BINASCII               (1)
// CRC32 + DEFLATE — enabled now that build_port.sh includes the
// uzlib sources (crc32.c, tinflate.c, lz77.c, defl_static.c,
// adler32.c, header.c). DEFLATE_COMPRESS adds the encoder side
// (defl_static.c + lz77.c) for `deflate.DeflateIO(buf, mode, ...)`
// in write mode.
#define MICROPY_PY_BINASCII_CRC32         (1)
#define MICROPY_PY_DEFLATE                (1)
#define MICROPY_PY_DEFLATE_COMPRESS       (1)
#define MICROPY_PY_HASHLIB                (1)
#define MICROPY_PY_HASHLIB_SHA256         (1)

// MD5 + SHA1 are gated on MICROPY_SSL_AXTLS in upstream modhashlib.c.
// We compile the real axtls library (upstream/lib/axtls/), so the
// AXTLS branch of modhashlib.c picks up its native MD5/SHA1 from
// crypto/md5.c and crypto/sha1.c. (Earlier the port shimmed the
// AXTLS API onto B-Con's public-domain hashes via
// uc386-dos/lib/axtls/crypto/crypto.h to get hashlib without TLS;
// that shim is now superseded by the real axtls build, but we keep
// the search path priority so it would still work if disabled.)
#define MICROPY_PY_HASHLIB_SHA1           (1)
#define MICROPY_PY_HASHLIB_MD5            (1)
#define MICROPY_SSL_AXTLS                 (1)

// Real TLS via axtls. modtls_axtls.c is the MicroPython glue,
// upstream/lib/axtls/{ssl,crypto}/ is the library itself. axtls's
// I/O is wired to mp_stream_posix_read/write (extmod/axtls-include/
// axtls_os_port.h) so reads/writes go through the lwIP socket's
// stream protocol — no separate BIO wrapper needed. Cert verification
// is OFF (CONFIG_SSL_CERT_VERIFICATION undef in the same header), so
// `import ssl` accepts whatever the server presents; this matches
// upstream MP's posture on resource-constrained ports. Use a CA
// chain via load_verify_locations once the bigger ROM budget
// allows — for now, server-auth is the user's responsibility.
#define MICROPY_PY_SSL                    (1)
// MICROPY_STREAMS_POSIX_API exposes mp_stream_posix_{read,write,lseek}
// in py/stream.c — axtls's SOCKET_READ/SOCKET_WRITE macros (in
// extmod/axtls-include/axtls_os_port.h) call those to bridge axtls
// I/O to the underlying lwIP socket. Without this gate the symbols
// are #ifdef'd out and the link fails.
#define MICROPY_STREAMS_POSIX_API         (1)

// lwIP — IPv4-only TCP/UDP/DNS stack. Phase 1 is loopback-only
// (no packet driver yet), so socket(AF_INET, SOCK_STREAM) can
// bind/listen/connect against 127.0.0.1 within a single REPL.
// MICROPY_PY_SOCKET turns on the BSD-style surface modlwip.c
// exposes; MICROPY_PY_LWIP itself just exposes the raw `lwip`
// module which we don't bother with separately. The hostname
// default is required by extmod/modnetwork.c — we don't actually
// build modnetwork.c (no MICROPY_PY_NETWORK), but modlwip.c
// references the hostname for DNS, so set it here.
#define MICROPY_PY_LWIP                   (1)
#define MICROPY_PY_SOCKET                 (1)
#define MICROPY_PY_NETWORK_HOSTNAME_DEFAULT  "uc386-dos"
#define MICROPY_PY_NETWORK_HOSTNAME_MAX_LEN  16

// modlwip.c's poll_sockets() drives this hook between events. With
// no real OS scheduler it's how blocking calls (DNS resolution,
// non-blocking-with-timeout connect, the udp_recv wait loop) drive
// netif RX / sys_check_timeouts forward. Empty default would deadlock
// any blocking lwIP call — `getaddrinfo` would never see its DNS
// response because the netif input loop never runs.
extern void uc386dos_loopback_poll(void *arg);
extern void sys_check_timeouts(void);
// Plain-statements form (no `do{}while(0)`): modlwip.c's call site
// is `MICROPY_PY_LWIP_POLL_HOOK\n  next_stmt;` with no trailing
// semicolon after the macro, so a `do{}while(0)` body would fall
// directly into the next statement and parse-error.
#define MICROPY_PY_LWIP_POLL_HOOK \
    uc386dos_loopback_poll(0); \
    sys_check_timeouts();

#define MICROPY_PY_RE                     (1)
#define MICROPY_PY_RE_SUB                 (1)
#define MICROPY_PY_RE_MATCH_GROUPS        (1)
#define MICROPY_PY_RE_MATCH_SPAN_START_END (1)
#define MICROPY_PY_HEAPQ                  (1)

#define MICROPY_ENABLE_COMPILER           (1)
#define MICROPY_ENABLE_GC                 (1)
#define MICROPY_HELPER_REPL               (1)

// We don't run mpy-tool.py, so the frozen-module symbols
// (mp_frozen_mpy_content, mp_frozen_names,
// mp_qstr_frozen_const_pool) don't exist; the minimal port's =1
// setting requires them as externs.
#define MICROPY_MODULE_FROZEN_MPY         (0)
#define MICROPY_ENABLE_EXTERNAL_IMPORT    (1)

#define MICROPY_ALLOC_PATH_MAX            (256)
#define MICROPY_ALLOC_PARSE_CHUNK_INIT    (16)

typedef long mp_off_t;

#define MICROPY_HW_BOARD_NAME "uc386-dos"
#define MICROPY_HW_MCU_NAME   "i386"

// `platform` module identity. modplatform.h's MICROPY_PLATFORM_*
// detections fall through to the generic "MicroPython" / "" case
// because our build doesn't define __linux / __GLIBC__ / etc.
// MICROPY_PLATFORM_VERSION IS `#ifndef`-guarded though, so we can
// stamp the runtime identity through that one.
#define MICROPY_PLATFORM_VERSION "uc386-dos i386 (PMODE/W)"

// Use the same STDOUT path the minimal port uses on linux/darwin —
// uc386's libc turns read(STDIN)/write(STDOUT) into INT 21h DOS calls.
#define MICROPY_MIN_USE_STDOUT (1)

// 256 KB GC heap. Bumped from 64 KB after the first runnable port:
// arbitrary input echoes back but the parser/compile path hits an
// UC_ERR_READ_UNMAPPED — heap exhaustion during parse-tree
// allocation is one likely cause. dos_emu maps a much wider data
// region than 64 KB so the additional fixed-region heap is fine.
#define MICROPY_HEAP_SIZE      (262144)

#define MP_STATE_PORT MP_STATE_VM
