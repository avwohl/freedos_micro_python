# MicroPython port — status: **full REPL @ EXTRA_FEATURES + lwIP DHCP + Crynwr packet-driver path + axtls TLS w/ cert verification** (2026-05-07)

**Upstream**: https://github.com/micropython/micropython
**License**: MIT

**`addons/gnu/micropython/build_port.sh` produces a runnable
`build/micropython.bin` (~444 KB at EXTRA_FEATURES + axtls TLS;
~408 KB pre-axtls; ~296 KB pre-lwIP-DHCP. Surface: `os` (incl.
listdir/stat/system/getenv/environ + port-supplied `os.path`
submodule) + `time` (incl. time_ns) + `random`/`binascii`/
`hashlib` (md5 + sha1 + sha256)/`re`/`cmath`/`heapq`/`deflate`/
`io`/`uctypes`/`json`/`platform`/`base64`/`shutil`/`tempfile`/
`ssl` (= `tls`) modules + LONGINT_LONGLONG + MICROPY_STACK_CHECK
+ EXACT float formatter + `help()` + module `__file__`. Was
~169 KB at MINIMUM, ~199 KB at CORE_FEATURES, ~263 KB at the
first EXTRA_FEATURES landing).** Run under `uc386.dos_emu.run`,
it boots the MicroPython REPL and accepts essentially full Python:

```
MicroPython uc386-triage on 2026-05-01; uc386-dos with i386
Type "help()" for more information.
>>> def fib(n):
...     if n < 2: return n
...     return fib(n-1) + fib(n-2)
...
>>> print([fib(i) for i in range(10)])
[0, 1, 1, 2, 3, 5, 8, 13, 21, 34]
>>> print(min(3, 1, 2), max(3, 1, 2))
1 3
>>> print(bin(255), hex(255), oct(255))
0b11111111 0xff 0o377
>>> try:
...     1/0
... except ZeroDivisionError:
...     print("caught")
...
caught
```

What works (70 smoke tests pin the core wins):

- arithmetic + control flow (`if/else`, `for/range`, `while/break`)
- function def + call (`def f(x): return x*2; f(7)` → `14`)
- recursion (`fib(10)` → `55`)
- classes (`class C: x = 1; C.x` → `1`)
- list/dict/tuple literals + comprehensions
  (`[i*i for i in range(5)]` → `[0, 1, 4, 9, 16]`)
- exception handling (`try/except` catching `ZeroDivisionError`)
- named builtins: `print`, `len`, `range`, `sum`, `sorted`, `zip`,
  `divmod`, `min`, `max`, `reversed`, `bin`, `hex`, `oct`, `abs`,
  `chr`, `ord`, `repr`, `type`, `isinstance`, `bytes`, `globals`,
  `bool`, `any`, `all`
- string operations + `str.format`/`str.upper`/`str.replace`/`str.join`
- C-style str modulo formatting (`'%d-%s' % (5, 'x')` → `5-x`)
- `set` literals + binary ops (`{1,2,3} | {3,4}` → `{1,2,3,4}`)
- `bytearray(b'abc')` — pulled in at CORE_FEATURES
- list slicing (`l[1:4]`)
- generator expressions (`','.join(str(i) for i in range(5))`)
- `dict.fromkeys` / `dict.get` / `bytes.decode`
- detailed error reporting — NameError now includes the offending
  qstr (`name 'undefined_name' isn't defined` vs MINIMUM's bare
  `name not defined`)
- `import sys` / `import gc` / `import micropython` /
  `import collections` (OrderedDict + namedtuple) /
  `import struct` / `import array` / `import errno` /
  `import math` / `import time` — registered via
  `build/genhdr/moduledefs.h`
  (hand-rolled equivalent of upstream's
  `tools/makemoduledefs.py` output, with `#if` guards
  mirroring each module's `MICROPY_PY_<X>` gate). `errno`
  additionally needs `MICROPY_USE_INTERNAL_ERRNO=1` (uc386's
  `<errno.h>` ships only the Linux subset, missing
  EOPNOTSUPP/EADDRINUSE/ECONN*/EHOST*/EALREADY/EINPROGRESS)
  plus build.sh's X-macro-aware grep for the EPERM/ENOENT/...
  qstrs that `MP_QSTR_##e` token paste needs.
- `MICROPY_FLOAT_IMPL=DOUBLE` + `MICROPY_PY_MATH_SPECIAL_FUNCTIONS=1`
  — uc386 lowers `double` through the x87 FPU.
  lib/i386_dos_libc.asm provides `sin`/`cos`/`tan`/`asin`/`acos`/
  `atan`/`atan2`/`sinh`/`cosh`/`tanh`/`asinh`/`acosh`/`atanh`/
  `exp`/`log`/`log10`/`log2`/`expm1`/`pow`/`sqrt`/
  `floor`/`ceil`/`trunc`/`fmod`/`modf`/`fabs`/`copysign`/`signbit`/
  `isnan`/`isinf`/`isfinite`/`nan`/`nearbyint`/`ldexp`/`frexp`/
  `erf`/`erfc` in raw 387 asm; `tgamma`/`lgamma` are NaN stubs.
  Hyperbolics use `e^±x` via the existing `_exp` (sinh/cosh/tanh)
  or sqrt+log identities (asinh/acosh/atanh). `log2` uses fyl2x
  with `y=1`. `expm1` uses `f2xm1` directly when |x*log2(e)| < 1
  (preserves precision near 0). `erf` uses Abramowitz & Stegun
  7.1.26 (5-term polynomial; ~1.5e-7 max error). `import math;
  math.sqrt(2.0)` → `1.41421356...` works end-to-end.
- `time` module — `time.ticks_ms`, `time.ticks_us`, `time.ticks_diff`,
  `time.ticks_add`, `time.sleep`, `time.sleep_ms`, `time.sleep_us`
  wired through INT 1Ah AH=00h (BIOS tick counter, ~18.2 Hz,
  ~55 ms/tick). lib/i386_dos_libc.asm:`_bios_ticks` makes the BIOS
  call, uc386-dos/mphal_uc386dos.c scales to ms/µs and busy-waits
  in `mp_hal_delay_ms`. `time.time` / `time.time_ns` are gated on
  `MICROPY_PY_TIME_TIME_TIME_NS` (default off — DOS lacks an
  always-running RTC integration). dos_emu emulates INT 1Ah AH=0
  with a synthetic monotonic counter so smoke tests can exercise
  the path without wall-clock timing.
- **EXTRA_FEATURES surface** — selectively opted into without
  taking the full ROM_LEVEL bump (which broke the value-print
  path under uc386 codegen): `compile()` / `eval()` / `exec()`,
  `input()`, `memoryview`, `next(it, default)` (`MICROPY_PY_BUILTINS_NEXT2`),
  `collections.deque` (with iter + subscr), `math.pi` / `math.e`
  / `math.tau` / `math.inf` / `math.nan`, `math.factorial`,
  `math.isclose`, class instance binary-op overrides via
  `MICROPY_PY_ALL_SPECIAL_METHODS` + INPLACE + REVERSE forms
  (`__add__` / `__iadd__` / `__radd__` / `__and__` / etc.),
  `bytes.hex` / `bytes.fromhex`, `str.center` / `str.partition`
  / `str.splitlines`, `frozenset`, **f-strings** (`f"x={val}"`),
  function attribute access (`f.__name__`), `delattr` / `setattr`,
  bytearray slice-assignment, plus REPL polish (Emacs key
  bindings + auto-indent + Ctrl-C `KeyboardInterrupt`).
  - `INFINITY` / `NAN` / `HUGE_VAL` in lib/include/math.h now use
    `__builtin_inf()` / `__builtin_nan("")` / `__builtin_huge_val()`.
    uc386's `_const_eval_float` recognizes these markers and folds
    them to IEEE-754 +inf / qNaN at compile time — the previous
    `(double)0x7FFFFFFF` / `(double)0` definitions were just
    integers cast to double, leading to `math.inf` = 2147483647.0
    and `math.nan` = 0.0. lib/i386_dos_libc.asm provides matching
    runtime stubs for the non-const path.
  - `MICROPY_PY_ALL_SPECIAL_METHODS` requires the dunders
    (`__add__`, `__sub__`, `__and__`, ...) to fit in 1-byte qstr
    ids because `mp_binary_op_method_name[]` (py/objtype.c:483)
    is a `const byte[]`. Our `gen_qstrdefs.py` now mirrors
    upstream's `static_qstr_list` + `unsorted_qstr_list` (from
    `tools/makeqstrdata.py`) and emits those qstrs as QDEF0
    entries with id < 256. **Caveat**: at DOUBLE without long-double
  precision, upstream's APPROX float formatter accumulates round-
  off across digit-extract multiplies and `print(4.0)` shows
  `3.999999999999997` instead of `4.0`. We patch
  `upstream/py/formatfloat.c` in build.sh to cap
  `MAX_MANTISSA_DIGITS` at 16 (= SAFE_MANTISSA_DIGITS) so the
  noise is bounded; full fix would require switching to the
  EXACT formatter on real long-double, which uc386 doesn't
  support today.
- static qstrs (`__name__` → `'__main__'`)
- `print()` with real newlines (qstr reverse-mangling correctly
  decodes `_brace_open__colon__hash_b_brace_close_` → `{:#b}`)
- clean Ctrl-D exit

- `import ssl` (and `import tls` — same module under both names)
  — full TLSv1 client + server via axtls (upstream/lib/axtls/,
  pinned to micropython/axtls @ 531cab9c). 15 axtls source files
  compile clean through uc386 (ssl/{asn1,loader,tls1,tls1_svr,
  tls1_clnt,x509}.c + crypto/{aes,bigint,crypto_misc,hmac,md5,sha1,
  sha384,sha512,rsa}.c). Library size: ~+50 KB on top of pre-SSL.
  The MP glue is `uc386-dos/modtls_axtls_uc386dos.c` — a fork of
  upstream's `extmod/modtls_axtls.c` that adds **real cert
  verification**: settable `verify_mode`, `SSLContext.load_verify_locations`,
  drop of `SSL_SERVER_VERIFY_LATER` when `CERT_REQUIRED`. Exposes
  `ssl.SSLContext`, `ssl.PROTOCOL_TLS_CLIENT/SERVER`, `ssl.CERT_NONE`,
  `ssl.CERT_REQUIRED`, `SSLContext.wrap_socket`, `.load_cert_chain`,
  `.load_verify_locations(cadata=...)`, `.verify_mode` (read+write).

  axtls's I/O is wired through `extmod/axtls-include/axtls_os_port.h`
  to `mp_stream_posix_read/write` (gated on `MICROPY_STREAMS_POSIX_API=1`,
  flipped in `uc386-dos/mpconfigport.h`), so reads/writes traverse
  the underlying lwIP socket via the standard stream protocol — no
  separate BIO needed.

  Cert verification is **on** —
  `CONFIG_SSL_CERT_VERIFICATION=1` and `CONFIG_SSL_HAS_PEM=1` are
  flipped by `fetch.sh`'s `patch_axtls_config_verify` post-fetch
  hook (idempotent sed against `upstream/extmod/axtls-include/
  config.h`). With `verify_mode=CERT_REQUIRED`, axtls's
  `x509_verify` runs during the handshake and fails it on chain
  validation errors. Required additions:
    - `lib/include/arpa/inet.h` (htonl/ntohl/htons/ntohs).
    - `lib/include/sys/time.h` (struct timeval).
    - `lib/i386_dos_libc.asm`: `_rand_r` (POSIX reentrant LCG —
      axtls's RNG_initialize stirs the entropy pool with it),
      `_strnlen` (axtls's x509.c uses it for SAN DNS-name length
      capping). `_time`, `_mktime`, `_gettimeofday` are now in
      `uc386-dos/time_real_uc386dos.c` (real DOS RTC + Howard
      Hinnant days-from-civil epoch math) — the libc stubs were
      removed because cert verification needs real epoch values
      to compare against the parsed notBefore/notAfter window.
    - `EWOULDBLOCK` alias in `lib/include/errno.h`.
    - **uc386 fix in `_mangle_static_globals`**: the multi-TU
      static-globals mangler was lexical-only, so a header's
      static inline parameter named `map` got rewritten to point
      at crypto_misc.c's `static const uint8_t map[128]` (the
      base64 decode table), turning a `mp_map_t *` arg into a
      `char[]`. Now scope-aware: parameter names and block-locals
      shield references from the rename.

  CA bundle: shipped per-context via Python (`load_verify_locations(
  cadata=PEM_BYTES)`). No system-wide bundle is baked in — pass the
  bytes you trust. The `test_micropython_ssl_load_verify_locations`
  smoke test exercises this with the ISRG Root X1 PEM as a known-good
  example.

  Library-level tests pass under dos_emu without a network rig
  (import, SSLContext construction, verify_mode round-trip,
  load_verify_locations parse-success and parse-failure).

  End-to-end wire test: `addons/gnu/micropython/tls-rig/` boots
  FreeDOS in QEMU with NE2000+SLIRP, runs MP.EXE under PMODE/W
  with TLSTEST.PY paste-fed through MP's REPL paste mode, and
  exercises a real TLS handshake against a host-side `tls_server.py`
  constrained to TLSv1.0 + AES128-SHA (the cipher set axtls
  speaks). CI driver: `.github/workflows/tls-rig.yml`.

  Current state — MP boots cleanly, REPL banner appears, paste
  mode delivers TLSTEST.PY in one shot, and execution begins
  (`TLSTEST: start`). The bisect markers identify exactly where
  it stops: `uc386_net.eth_init()` returns -1 because
  `pktdrv_detect` fails. Root cause: under PMODE/W on real DOS,
  a bare `INT 0x60` from protected mode goes through the IDT,
  not the real-mode IVT — so the Crynwr packet driver (loaded
  via NE2000.COM at INT 0x60 in DOS real mode) isn't reached.
  The byte-signature scan also fails because conventional memory
  isn't always mapped at the linear address the seg*16+off math
  assumes under PMODE/W's paging.

  Partial fix landed in `uc386-dos/pktdrv_uc386dos.c`: probe
  candidate IVT slots via DPMI fn 0x0300 (Simulate Real Mode
  Interrupt) calling Crynwr's `driver_info` (AH=0x01). That gets
  detection working under PMODE/W. The remaining pktdrv calls
  (`pktdrv_access`, `pktdrv_get_addr`, send/recv) still use the
  bare-INT path and ALSO need to route through DPMI 0x0300, plus
  buffers must live in conventional memory (allocated via DPMI
  fn 0x0100 + bounce-copy back to flat-32 buffers). That's the
  open work item — meaningful but non-trivial DOS-extender
  plumbing, ~1-2 days. The CI step is `continue-on-error: true`
  while that lands.

  Not blocking on TLS itself: the library, smoke tests, and rig
  infrastructure are all in place. Once pktdrv works under
  PMODE/W, the TLS handshake should "just work" — axtls's I/O
  is wired through `mp_stream_posix_read/write` over the lwIP
  socket, which gets its packets through the same pktdrv path
  the existing dosbox-x-rig already validates under DOSBox-X.

  Side note: there's a second, smaller mystery in the QEMU+FreeDOS
  output — `repr(-1)` returns the string "11111111111111111111111"
  (23 ones) instead of "-1". `str(0)` / `str(255)` etc. all work
  correctly under dos_emu. Likely a uc386 codegen bug that only
  manifests under PMODE/W's BSS layout. Diagnostic-only; doesn't
  block the wire test once eth_init returns 0.

What doesn't work yet (separate gates, pinned in mpconfigport.h):

- `import cmath` — needs `MICROPY_PY_CMATH`; we have float math
  but no complex-number support today. Adding it is mostly a
  matter of opting in plus the few extra qstrs it needs.
- Full `tgamma` / `lgamma` — currently NaN stubs in libc. A real
  Lanczos approximation is the EXTRA_FEATURES follow-up.
- `open()` + `import xxx` from disk — file I/O wired through
  uc386's libc INT 21h syscalls. Port-supplied `mp_builtin_open_obj`
  / `mp_import_stat` / `mp_lexer_new_from_file` in
  `uc386-dos/file_uc386dos.c` — no full VFS, just enough for
  flat .py imports and a read/write file object that supports the
  full mp_stream protocol (read/readinto/readline/write/close/
  seek/tell/flush/__enter__/__exit__). Required `_stat` + `_fstat`
  asm in libc that reassembles the full 32-bit DX:AX position
  return from INT 21h AH=0x42 SEEK_END (was inheriting stale
  upper-16 bits of EAX, leading to multi-MB phantom file sizes).
  Required `MICROPY_PY_IO=1` in mpconfigport.h. Stub overrides of
  `mp_import_stat` / `mp_lexer_new_from_file` in
  `upstream/ports/minimal/main.c` are sed-patched out by build.sh
  so our real implementations link cleanly.

- `os.system(cmd)` / `os.getenv(name, default=None)` /
  `os.environ()` — env-block walk via INT 21h AH=0x62
  (Get PSP) → PSP[0x2C] linear address. lib/i386_dos_libc.asm
  provides `_dos_get_psp_seg`, `_dos_env_base`, `_getenv`,
  `_dos_env_iter`, `_dos_argv0`. Under PMODE/W's flat 32-bit
  selectors, `seg << 4` directly addresses the env block in
  conventional memory. dos_emu builds a fake PSP at
  0x000F0000 with PSP[0x2C] = env_seg and the env populated
  from `run(env={...})`; `_dos_argv0` returns the trailing
  program-path string DOS 3.0+ writes after the env terminator.
  `os.environ()` is a function (not a property) — MicroPython
  modules don't support attribute-getter delegation, so we
  expose a fresh-snapshot dict on call. `os.system(cmd)` calls
  libc `system()` which spawns COMMAND.COM /C `cmd` via INT 21h
  AH=0x4B sub 0 (LOAD AND EXECUTE) and reads the exit code via
  AH=0x4D. dos_emu's handler recognizes ECHO and EXIT N for
  smoke testing; real DOS shells out through whatever path
  COMSPEC points at.

- ~~Full `MICROPY_CONFIG_ROM_LEVEL = EXTRA_FEATURES`~~ — DONE
  (2026-05-03). The wholesale-EXTRA hang turned out to be
  `MICROPY_STACK_CHECK`: it needs the port to call
  `mp_stack_set_top()` / `mp_stack_set_limit()` at init, which
  ports/minimal/main.c (our entry point) doesn't do. Without
  those calls every check fails, and the stack-overflow raise
  path infinite-loops. mpconfigport.h now sets
  `MICROPY_STACK_CHECK=0` while keeping the wholesale ROM bump.
- `import _thread` / `import weakref` — `MICROPY_PY_THREAD` and
  `MICROPY_PY_WEAKREF` not enabled at CORE_FEATURES.
- `MICROPY_PY_IO` (open/io machinery) — port has no VFS.
- Class instance binary-op overrides (`__and__` etc.) — gated at
  `MICROPY_PY_ALL_SPECIAL_METHODS` (EXTRA_FEATURES) which we don't
  enable today, but the qstrs decode correctly so a future bump is
  ready.
- Full `MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES` not yet
  attempted; CORE_FEATURES is now the baseline.

## Update 2026-05-02 — CORE_FEATURES regression resolved

The "every named-builtin NameErrors at CORE_FEATURES" runtime
regression was not the static-init bug we suspected. Root cause:
**`MICROPY_QSTR_BYTES_IN_HASH` flips from 0 (MINIMUM) to 1
(CORE_FEATURES)**, which adds a `qstr_hash_t hashes[]` array to
`qstr_pool_t` and gates `qstr_find_strn`'s post-binary-search
filter on `pool->hashes[at] == str_hash`. Our `gen_qstrdefs.py`
was emitting `0` for every QDEF1's hash field; the runtime
computed real djb2 hashes for the lookup string, the filter
rejected every entry, and `print` / `min` / `__name__` raised
NameError.

Fix (`gen_qstrdefs.py` + `build.sh`): compute the djb2 hash
inline at qstrdefs-generation time, mirroring upstream's
`tools/makeqstrdata.py:compute_hash` (including the
`(hash & mask) or 1` zero-fix), and emit it as the second
argument of `QDEF1(...)`. New `--bytes-hash N` arg picks the
mask width to match `MICROPY_QSTR_BYTES_IN_HASH` —
`build.sh` passes `--bytes-hash 1` (matches CORE_FEATURES).
Pinned by a 28-case golden test against upstream's own
implementation. Extra 4 smoke tests cover the surface
CORE_FEATURES newly enables: `bytearray`, `set`,
detailed NameError text, and C-style `%` string formatting.

## Historical investigation (2026-05-01 morning)

The text below traces how the REPL went from "boots banner" to
"runs `pass`" to "runs arithmetic" to "evaluates Python". Most of
it is no longer load-bearing for current behavior but documents
the diagnostic path for future similar bring-ups.

**Crash narrowed to `qstr_find_strn`'s binary search.** Diagnostic
hook at the dos_emu level pinpoints the failing instruction:
`push [eax + ecx*4]` at EIP 0x0001b8b9 inside `_qstr_find_strn`
(NASM listing line 44389; function starts at 0x0001b84d, line
44352). The crash address is `0x80024fc4`; `eax = 0x00024fc8`
(= `&pool->qstrs[0]`, so `pool` lives at `0x24fb4` — inside the
GC heap, i.e. a dynamically-allocated pool, not one of the const
pools at `0x13A4` / `0x13B8`). `ecx = 0x1fffffff`, the binary-
search midpoint `(high + low) / 2` from
`(pool->len - 1 + 0) / 2`, which means `pool->len` was read as
`0x40000000` — clearly garbage; a freshly-allocated dynamic
pool's `len` should start at 0.

The constant pools have correct layout in the assembled image
(verified from the NASM listing: prev / bitfield-word / alloc=10
/ len / lengths / qstrs[]; the bitfield word for
`mp_qstr_const_pool` reads `0x8000036F` = `is_sorted=1` packed
with `total_prev_len=879`). Reads from `pool->len` at offset 12
match the layout. So either:

- the dynamic pool's `len` is being clobbered by something between
  allocation and read (heap layout overlap, double-free, GC
  metadata trampling), or
- uc386's `sizeof(qstr_pool_t)` differs from the access offset
  uc_core uses — a few bytes of padding mismatch on the FAM
  (`const char *qstrs[]`) would shift fields, or
- a struct-identity collision is making two distinct structs look
  the same to uc_core's structural fingerprinting.

Suspects worth checking next:

- **dead libm externs**: 18 libm symbols (acos, sin, cosh, trunc,
  …) appear as externs but only as string-table debug names;
  they're listed because the asm DCE can't prove they're unused.
  An indirect call through a function pointer that resolves to one
  of these `extern _xxx` symbols would jump to the import-stub
  address instead of real code.
- **ast-optimizer copy-prop on a function pointer**: similar to
  the `void *data → struct *p` bug fixed earlier in this slice,
  but for function pointers — uc_core might be copy-propagating
  a function-table indirection that loses the call target.
- **Qstr-pool growth path**: `qstr_add` allocates via
  `m_malloc` from the GC heap. If GC's free-list traversal trips
  on something (unaligned heap base, off-by-one in the heap end
  sentinel), the allocation returns garbage.
- **Struct identity for nested unions**: `mp_obj_t` is a union
  of pointers and small-int-tagged ints; if the structural
  fingerprinting that resolves anonymous structs is collapsing
  two distinct union shapes, member access through the wrong
  variant would dereference garbage.

## Update 2026-05-01 — `x = 5` works, value-print crashes elsewhere

Two more fixes shipped (commits 21dc0d9 + ad0ad62):

1. **QDEF1 routing**: the grep'd qstrs were all going to QDEF0
   (static pool). `mp_qstr_const_pool` (the main, sorted pool) ended
   up with `len = 0` BUT `is_sorted = true` — the binary search in
   `qstr_find_strn` underflowed `pool->len - 1` to 0xFFFFFFFF and
   walked off the end of the address space. Routing every grep'd
   qstr to QDEF1 instead of QDEF0 made the main pool non-empty and
   unblocked the qstr lookup path used by `x = 5` (qstr-store).

2. **qstr name extraction off-by-one**: the awk that strips the
   `MP_QSTR_` macro prefix dropped 7 chars instead of 8, leaving
   a stray leading underscore on every qstr's string. So
   `MP_QSTR___repl_print__` was recorded with string
   `___repl_print__` (3 leading underscores), making
   `mp_load_name(MP_QSTR___repl_print__)` fail at the dict lookup
   stage. Fixed: `substr(s, 9)` instead of `substr(s, 8)`.

After both fixes, `pass`, empty line, `x = 5`, Ctrl-D-only all
exit cleanly. **Value-print** (`1`, `print('hi')`) still crashes
with `UC_ERR_READ_UNMAPPED`. Further narrowing via dos_emu hooks
on the linear-search loop in `mp_map_lookup`:

- The crashing dict has `table = 0` (NULL pointer) and a
  `top = 0x69358` value, implying `used*8 = 0x69358`, i.e.
  `used ≈ 53867`. The bitfield word at offset 0 of map is reading
  as some large garbage value (with bit 2 = is_ordered = 1, since
  the linear path was entered).
- This isn't `mp_module_builtins.globals` (its static layout is
  clean: bitfield = 0x267, alloc = 76, table = valid address).
- Likely `dict_main` (the `__main__` module's globals) — created
  at runtime by `mp_obj_dict_init(&dict, 1)` → `mp_map_init`.
  If uc386 emits the body of mp_map_init in a way that doesn't
  initialize the bitfield word cleanly, the existing memory at
  &dict_main.map (in BSS, so zero-initialized at boot) might be
  fine, but writing `used = 0` followed by `all_keys_are_qstrs = 1`
  could clobber bits in unexpected ways.

uc386's bitfield read + write generated code looks correct in
isolated tests (verified with focused `_compile` snippets — read
shifts and masks the right ranges; write does proper RMW).

**Further narrowing (final state of the session)**: hooked the
3 `mp_map_lookup` entry points and dumped `(map_ptr, index, kind,
return_addr)` for each. Result for input `1\n\x04`:

  - Call 1: ret=0xECD5, map=0x69360, index=0x21A (`__name__`),
    kind=1 (ADD_IF_NOT_FOUND). This is `mp_init`'s store of
    `__name__` into `dict_main`. dict_main lives in mp_state_ctx
    BSS; its `&dict_main.map` runtime address is 0x69360.
  - Call 2: ret=0x1C73F, map=0x69360, kind=0. This is
    `mp_load_name`'s `mp_locals_get()->map` lookup. Same dict_main —
    locals == globals at module scope, so this is correct.
  - Call 3: ret=0x1C773, map=**0x2A354**, kind=0. This is
    `mp_load_global`'s `mp_globals_get()->map` lookup *immediately
    after* the locals miss. **The map address differs from
    dict_main** (0x69360) — `mp_globals_get()` returned a different
    dict than `mp_locals_get()` despite mp_init's
    `mp_locals_set(&dict_main); mp_globals_set(&dict_main)`.

The map at 0x2A354 lives in the GC heap and is corrupted:
`bitfield = 0x6935C` (is_ordered=1, used=0xD26B), `alloc=121`,
`table=NULL`. The linear-search loop entered (because is_ordered=1)
walks from address 0 (`table`) for `used*8` bytes, immediately
hits boot-instruction bytes interpreted as mp_obj_t key/value
pairs, and trips the unmapped read at the first qstr-shaped
garbage value with high bit set.

**Root cause identified (in upstream-side codegen, not the
runtime)**: looking at the asm uc386 emits for `objfun.c`'s
`fun_bc_call`:

```c
mp_globals_set(self->context->module.globals);
```

uc386 emits:
```
mov eax, [ebp - 4]   ; eax = self
push [eax + 4]        ; push self->context (the pointer VALUE)
call mp_globals_set
```

This has **only one dereference** — it pushes the *value of the
context pointer field* (`self->context`), NOT the result of
`self->context->module.globals` (which is two derefs deep).
The CORRECT asm would be:

```
mov eax, [ebp - 4]    ; self
mov eax, [eax + 4]    ; eax = self->context  (deref 1)
push [eax + 4]        ; push self->context->module.globals  (deref 2)
```

So `mp_globals_set(...)` ends up storing the *context pointer
itself* as the new globals dict. mp_globals_get later returns
that context pointer. The runtime treats it as a `mp_obj_dict_t *`,
and `&dict.map = context+4 = 0x2A354` (the corrupt-looking dict
we saw in our hooks).

Isolated `_compile`-driven reproductions of the same access
pattern (struct base; pointer member; embedded struct; pointer
field) all generate the correct two-deref asm. The bug only
triggers in the actual MicroPython multi-TU build — likely a
struct-identity collision in uc_core's anonymous-struct
fingerprinting that loses track of `mp_obj_module_t` vs
`mp_module_context_t` across the relevant TUs (objfun.c sees
slightly different types than runtime.c does), so the
`->module.globals` traversal stops one deref short.

Fix is in **uc_core**, not uc386 — needs source-level investigation
of how multi-TU struct fingerprinting interacts with embedded
structs whose first field is itself a struct (`mp_obj_base_t base`).
Outside the scope of this repo.

  - EIP 0xCC41, inside `mp_obj_equal_not_equal`, instruction
    `cmp dword [eax], _mp_type_str`.
  - lhs (the `[ebp+12]` argument, which is the second of three:
    op, lhs, rhs) = garbage pointer (e.g. 0xd9027f24, 0xec835053).
  - rhs = qstr-tagged value e.g. 0x027a (qstr id 79 in our table,
    which is `__rand__`).
  - Caller is `mp_map_lookup`, which is walking dict entries and
    comparing each entry's key against the lookup key. The garbage
    lhs comes from one of the dict entries.

Hypothesis: a global dict (probably `mp_module_builtins.globals`
or similar) has a corrupt entry whose key is a bad pointer. The
qstr id 79 (`__rand__`) being the lookup key suggests this is the
bytecode compiler probing for `__rand__` (right-AND) during
constant folding or peephole-opt of `1`. uc386's static
initializer for one of the registered-builtins dict tables is
likely emitting wrong bytes for an mp_obj_t — possibly a
struct-identity collision where two `mp_obj_t` union variants
get the same fingerprint.

## Build

```sh
./fetch.sh    # clones micropython upstream into upstream/
./build.sh    # per-file triage of upstream/py/*.c +
              # upstream/shared/{libc,readline,runtime}/ through
              # uc386. Writes build/<name>.asm on PASS,
              # build/<name>.err on FAIL; build/triage.txt is the
              # per-source ledger, build/errors.txt the histogram.
```

## Triage result (latest run)

```
== triage: 145 pass / 0 fail / 145 total ==
    py/                                          132 / 132
    shared/{libc,readline,runtime,timeutils,netutils}/  13 / 13
```

That's **100 % of the platform-independent core + every shared
support source a real port pulls in** compiling clean through
uc386 → NASM-ready .asm in one pass. The remaining work to land
an actual `micropython.bin` is the port shim
(`ports/uc386-dos/main.c` + `mphalport.c` + `mpconfigport.h`),
the GC heap region, and the multi-file link — none of which the
triage exercises.

The shared/ sources covered are:

- `shared/libc/printf.c`, `string0.c`, `__errno.c`, `abort_.c`
  (the minimal-port libc)
- `shared/readline/readline.c` (REPL line editor)
- `shared/runtime/pyexec.c` (REPL driver), `stdout_helpers.c`,
  `interrupt_char.c`, `sys_stdio_mphal.c`
- `shared/timeutils/timeutils.c` (date/time helpers used by most
  ports)
- `shared/netutils/netutils.c`, `trace.c`, `dhcpserver.c`
  (network + DHCP server helpers used by richer ports —
  rp2/esp32 pull these in)

The setup:

- Stub `genhdr/moduledefs.h` (defines
  `MICROPY_REGISTERED_MODULES` and
  `MICROPY_REGISTERED_EXTENSIBLE_MODULES` empty), `genhdr/mpversion.h`
  (placeholder strings).
- Auto-generate `genhdr/qstrdefs.generated.h` by grepping
  `MP_QSTR_*` references out of `upstream/py/` and
  `upstream/shared/`, emitting the matching `QDEF0(...)` macro
  invocations. Approximates upstream's `tools/makeqstrdefs.py`
  over-inclusively (any MP_QSTR_x pattern becomes a qstr, even
  if it's only a comment in real source) but keeps the enum in
  `py/qstr.h` complete enough that downstream refs resolve.
- Auto-generate `genhdr/root_pointers.h` by grepping
  `MP_REGISTER_ROOT_POINTER(<decl>);` declarations out of py/ +
  shared/ and emitting them as struct fields. Approximates
  upstream's `makeqstrdefs.py mode=root_pointer` — needed because
  `py/mpstate.h` `#include`s this header inside `_mp_state_vm_t`
  to grow per-module root-pointer fields (e.g. `readline_hist`,
  `mp_sys_argv_obj`).
- Synthetic `int main()` so uc386's "every TU needs `main`" check
  accepts library sources.

All py/*.c sources now compile. Earlier triage runs showed one
last failure on `objmodule.c`'s `mp_builtin_module_table[]`
initializer expanding the undefined `MICROPY_REGISTERED_MODULES`
macro into a literal identifier (`got Identifier
MICROPY_REGISTERED_MODULES`); the stub `genhdr/moduledefs.h` now
defines that macro (and `MICROPY_REGISTERED_EXTENSIBLE_MODULES`)
as empty, matching the shape a real port-without-modules would
emit through `py/makemoduledefs.py`.

## Bug surfaced (and fixed)

The `pp->m`-on-a-typedef case surfaced a real bug in the
**uc_core AST optimizer's copy-propagation path**. The shape that
tripped it up:

```c
void f(void *data) {
    struct printer *pr = data;     // legal C: void* → struct*
    if (pr->flag) { ... }           // ← optimizer rewrote pr → data
}
```

`_types_compatible_for_copy` happily propagated `pr = data` because
both sides are PointerType. But replacing `pr` with `data` loses the
declared `struct printer *` type — `_type_of(data)` returns
`PointerType(void)`, which uc386's `->` lowering rejects.

**Fix** (in `uc_core/src/uc_core/ast_optimizer.py`): refuse copy
propagation between two PointerTypes when either side's pointee is
`void`, or when the pointee kinds differ (one BasicType, one
StructType, etc.). Equivalent pointers (e.g. `int *` to `int *`)
still propagate.

**Triage progression**:
- 95/132 with empty qstrdefs (most failures were downstream of
  missing MP_QSTR enum entries, not separate bugs).
- 115/132 once the synthetic qstr table was in place.
- 117/132 with the uc_core copy-prop fix lifting the 2 `pp->m`
  failures.
- 130/132 once `_const_eval` learned `TernaryOp` (lifted the 12
  packed-flag `.sig` failures from `MP_OBJ_FUN_MAKE_SIG`'s
  `(takes_kw) ? 1 : 0` ternary).
- 131/132 (current) once `_resolved_var_type` learned to const-
  eval enum-constant designators (lifted the
  `[SCOPE_GEN_EXPR] = ...`-style array-size mis-inference).

## Next steps for a runnable image

The triage proves the core is reachable. To land an actual
`micropython.bin`:

1. **Run upstream's `tools/makeqstrdefs.py`** to emit the real
   `genhdr/qstrdefs.generated.h` (correct hash + len fields,
   minus the over-inclusion the grep heuristic ships).
2. **Compiler fixes for MicroPython idioms** — all already shipped
   as part of this slice:
   - **uc_core**: copy-propagation refuses propagation across
     `void *` and across pointee-kind boundaries (was rewriting
     `void *data → struct *p` propagations and losing struct
     type for later `p->m`).
   - **uc386 const-eval**: `TernaryOp` + comparison + `&&`/`||`
     now fold (lifted the `MP_OBJ_FUN_MAKE_SIG`'s `.sig` family).
   - **uc386 array sizing**: `_resolved_var_type` const-evals
     enum-constant designators when inferring an unsized array's
     length (lifted the `static const T arr[] = { [ENUM] = … }`
     class).
3. **Write `ports/uc386-dos/`** — a thin port with:
   - `mpconfigport.h` (start from `ports/minimal/`)
   - `main.c` calling `mp_init` / `pyexec_friendly_repl` with
     a fixed-region heap.
   - `mphalport.c` — `mp_hal_stdout_tx_strn` → INT 21h AH=09;
     `mp_hal_stdin_rx_chr` → INT 21h AH=01; `mp_hal_ticks_ms`
     → INT 1Ah BIOS time.
   - GC-aware setjmp/longjmp; uc386 already lowers them via
     libc, but the GC root scan needs to know where the stack
     range is — the port shim wires that up via
     `MP_STATE_THREAD(stack_top)`.
4. **Compile + link multi-file** through uc386, using the same
   pattern as the doom port (single uc386 invocation over the
   whole TU set). Existing multi-file affordances (file-scope
   `static` mangling, structural anonymous-struct identity, etc.)
   are already in place.

## Build artefacts

`build/` contains per-source `.asm` (one per PASS), `.err`
(stderr per FAIL), and the two roll-ups `triage.txt` and
`errors.txt`. None of these ship in the release tarball — they
are dev-side intermediate output.

## License

MIT — see `upstream/LICENSE` after running `./fetch.sh`. The thin
uc386 port shim, when written, inherits GPL-3.0 from the parent
uc386 repo (matches the convention used by the in-tree GNU
utility addons).
