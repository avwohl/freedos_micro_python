# freedos_micro_python

[MicroPython](https://github.com/micropython/micropython) port for
**FreeDOS / i386**, built end-to-end through the
[uc386](https://pypi.org/project/uc386/) C23 compiler. Produces a
runnable flat-binary or PMODE/W `.exe` with a fully-functional
Python REPL — arithmetic, control flow, classes, list comprehensions,
exception handling, and ~25 named builtins all work.

```
MicroPython uc386-triage on 2026-05-01; uc386-dos with i386
Type "help()" for more information.
>>> def fib(n):
...     if n < 2: return n
...     return fib(n-1) + fib(n-2)
...
>>> print([fib(i) for i in range(10)])
[0, 1, 1, 2, 3, 5, 8, 13, 21, 34]
```

## Status

- ~444 KB binary at the EXTRA_FEATURES + axtls TLS configuration
- ~70 smoke tests pin REPL banner, builtins, comprehensions, exceptions,
  module imports (`os`, `time`, `re`, `json`, `hashlib`, `ssl`, ...),
  and the long-int / float code paths
- See [`NOTES.md`](NOTES.md) for the full per-slice development log

## Install

```
pip install freedos_micro_python
```

This pulls in `uc386` (the compiler) automatically. You also need:

- a Unix-y shell to drive the `build_port.sh` script (macOS / Linux)
- `git` (for fetching the upstream MicroPython sources)
- `make` is **not** required

## Quick start

```bash
mkdir mp-build && cd mp-build
freedos-micropython fetch        # clones upstream MicroPython into ./upstream
freedos-micropython build        # per-TU triage build (generates qstrdefs)
freedos-micropython port         # multi-TU build → ./build/micropython.bin
```

Wall-clock for the `port` step is ~14 minutes on a recent Mac. The
output is `./build/micropython.bin`, a flat i386 DOS binary runnable
under uc386's emulator:

```python
from uc386.dos_emu import run
res = run("build/micropython.bin", timeout_seconds=10.0,
          instruction_limit=2_000_000_000)
print(res.stdout)   # → "MicroPython uc386-triage on ...\n..."
```

To produce a real DOS `.exe` (PMODE/W bound, ~12 KB stub overhead):
use uc386's `addons/harness/exe.py`.

## Testing

After a successful `port` build:

```bash
pytest --pyargs freedos_micro_python    # parametric: tests live in tests/
# or, against a checkout:
pytest tests/
```

The smoke tests skip cleanly if `build/micropython.bin` doesn't exist.

## Bundled networking utilities

The port ships three pure-MicroPython programs that double as
regression tests and as usable standalone tools — drop them into a
DOS image (or run them in the REPL) and they work end-to-end against
real servers.

- **[`examples/wget.py`](examples/wget.py)** — HTTPS streaming
  downloader. Built on `socket` (lwIP-backed) and `ssl` (axtls
  CERT_REQUIRED supported via `--ca-certs`). Streams in 4 KB chunks
  so the whole body never sits in RAM. Follows up to 5 redirects.
  ```
  MP.EXE WGET.PY -O OUT.TXT https://example.com/file
  ```

- **[`examples/scp.py`](examples/scp.py)** — SCP client wrapping
  `_ssh.Session.scp_recv()` and `_ssh.Session.scp_send()` (which
  bind libssh2's `scp_recv2` / `scp_send_ex`). Password auth only
  for now; up/down inferred from which arg has the `host:/path` colon.
  ```
  MP.EXE SCP.PY user@10.0.2.2:/etc/motd MOTD.TXT
  MP.EXE SCP.PY DATA.BIN user@10.0.2.2:/uploads/data.bin
  ```

- **[`examples/sftp.py`](examples/sftp.py)** — SFTP client wrapping
  `_ssh.Session.sftp()` + `SFTP.open()` / `SFTPFile.read|write|close`.
  ```
  MP.EXE SFTP.PY get user@10.0.2.2:/etc/hostname HOST.TXT
  MP.EXE SFTP.PY put REPORT.TXT user@10.0.2.2:/incoming/report.txt
  ```

All three run inside the SSH rig harness (`rigs/ssh-rig/`,
`rigs/tls-rig/`) against a paramiko/local-server fixture and confirm
PASS end-to-end; see [`docs/TESTS.md`](docs/TESTS.md) for the full
catalog.

## Layout

- `src/freedos_micro_python/scripts/` — the three shell scripts
  (`fetch.sh`, `build.sh`, `build_port.sh`); invoked via the CLI
  wrapper, which sets `UC386_LIB_INCLUDE` from the installed `uc386`
- `src/freedos_micro_python/port/` — the FreeDOS port files
  (`mpconfigport.h`, `*_uc386dos.c`, lwIP + axtls glue)
- `src/freedos_micro_python/gen_qstrdefs.py` — qstr table generator
  (mirrors upstream's `tools/makeqstrdata.py`)
- `src/freedos_micro_python/cli.py` — the `freedos-micropython` CLI
- `examples/` — standalone MicroPython programs (`wget.py`, `scp.py`,
  `sftp.py`) shipped as both regression tests and usable utilities
- `tests/` — pytest smoke tests + qstr unit tests
- `rigs/dosbox-x-rig/` — DOSBox-X regression rig (network packet driver)
- `rigs/tls-rig/` — axtls TLS regression rig
- `rigs/ssh-rig/` — paramiko-fixture SSH/SFTP/SCP rig

## A debt to FreeDOS

This project targets [FreeDOS](https://www.freedos.org/). FreeDOS is
the reason a 32-bit i386 Python REPL on a 1990s-era PC makes any
sense in 2026 at all — without a maintained, open-source DOS kernel
+ shell + utilities, there'd be no plausible host for this binary
to run on.

We mostly use FreeDOS *as a target*: the rigs boot a stock FreeDOS
1.4 MB floppy image into QEMU (or DOSBox-X), run `MP.EXE` against
its kernel + COMMAND.COM + PMODE/W, and tear down. We do not
modify the FreeDOS kernel or utilities. But debugging PMODE/W's
INT 21h reflection, the DOS packet-driver interface, the FAT
write path, and a handful of NLS / RTC quirks would have been
impossible without the FreeDOS source tree to read.

In the spirit of paying that debt forward, the `release/` directory
ships a copy of the FreeDOS sources we leaned on, regardless of
whether our limited use strictly requires source redistribution
under their license. See [`release/README.md`](release/README.md)
for the catalog. License + copyright notices for FreeDOS and every
other third-party project bundled or fetched by the build are in
[`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md).

## License

[MIT](LICENSE), matching upstream MicroPython. The integration glue
(scripts, port files, CLI, tests) is what's covered here. Third-party
sources fetched by `build_port.sh` (MicroPython, axtls, lwIP,
libssh2, TweetNaCl, crypto-algorithms) retain their own licenses;
the FreeDOS sources in `release/` retain GPLv2 / their own
per-project licenses. The full catalog with attributions is in
[`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md).

## Related projects

- [FreeDOS](https://www.freedos.org/) — the target OS.
- [uc386](https://github.com/avwohl/uc386) — the C23 compiler that
  builds this port. Hosts the `dos_emu` test harness.
- [uc_core](https://github.com/avwohl/uc_core) — shared C23 frontend
  used by uc386 (and the Z80 sibling, uc80).
- [MicroPython](https://github.com/micropython/micropython) — upstream.

## MicroPython feature matrix

Settings come from
[`src/freedos_micro_python/port/mpconfigport.h`](src/freedos_micro_python/port/mpconfigport.h).
The port runs at `MICROPY_CONFIG_ROM_LEVEL = EXTRA_FEATURES`, the
richest preset upstream ships.

### Enabled

The EXTRA_FEATURES preset itself turns on the language-surface knobs
listed first; everything below it is an explicit override on top.

    Language surface (from EXTRA_FEATURES)
      compile() / eval() / exec()           input()
      memoryview                            frozenset
      f-strings                             collections.deque + iter/subscr
      __add__ / __radd__ / __iadd__ etc.    function attribute access
      delattr() / setattr()                 math.pi / e / tau / inf / nan
      math.factorial / math.isclose         bytes.hex / fromhex
      str.center / partition / splitlines   bytearray slice-assign
      Emacs REPL keys + auto-indent         Ctrl-C → KeyboardInterrupt

    Runtime
      ENABLE_COMPILER         ENABLE_GC               HELPER_REPL
      ENABLE_EXTERNAL_IMPORT  STACK_CHECK             NLR_SETJMP
      MODULE___FILE__         PY_BUILTINS_HELP        PY_BUILTINS_RANGE_BINOP
      USE_INTERNAL_ERRNO      STREAMS_POSIX_API

    Numerics
      FLOAT_IMPL  = DOUBLE          (full x87 double-precision)
      FLOAT_FORMAT_IMPL = EXACT     (round-trip shortest decimal)
      LONGINT_IMPL = LONGLONG       (heap-allocated big ints)
      PY_MATH_SPECIAL_FUNCTIONS     (erf, gamma, ...)
      PY_MATH_{ATAN2,FMOD,MODF,POW,GAMMA}_FIX  (CPython-matching edges)

    Standard library (extmod)
      io          open(), IOBase, BytesIO, StringIO
      sys         modules / exit / path / argv / exc_info / tracebacklimit
      time        time / time_ns / sleep_ms / ticks_ms / localtime / gmtime / mktime
      random      EXTRA_FUNCS, seeded from BIOS tick counter
      hashlib     SHA-256 + SHA-1 + MD5 (real axtls implementations)
      binascii    full surface incl. CRC32
      deflate     uzlib decoder + DEFLATE_COMPRESS encoder
      re          + sub, match groups, span/start/end
      heapq, json, struct, uctypes, select, _asyncio
      machine     mem8/mem16/mem32 (direct linear-address poke in PMODE/W)

    Networking + crypto (the harder lift)
      socket      BSD-style via lwIP (TCP/UDP/DNS, IPv4)
      ssl         axtls (handshake + CERT_REQUIRED with --ca-certs)
      _ssh        libssh2 1.11.1 over axtls + TweetNaCl
                  Session.userauth_password / exec / sftp() /
                  scp_recv / scp_send / close
      uc386_net   NE2000 packet driver, eth_init / eth_status / eth_set_static
      lwip        lwIP raw module (RX poll, callbacks)
      pktdrv      DOS packet-driver INT 60h harness
      dosint21    raw INT 21h access for DOS-native syscalls

### Not implemented

    _thread / PY_THREAD               DOS is single-threaded; emulating
                                      pre-emptive threads would mislead.
                                      Cooperative asyncio runs fine.

    cmath / PY_CMATH                  Complex numbers — not on the path
                                      for any user we serve today.

    weakref / PY_WEAKREF              Off at CORE; default at EXTRA.
                                      Skipped: no concrete use yet.

    VFS / PY_VFS                      MICROPY_VFS abstraction (mount,
                                      multiple FS backends) — we have a
                                      flat-file import path through INT
                                      21h instead. Adding VFS would buy
                                      FAT/Lit/Posix mounts and overlay
                                      semantics; not a current need.

    network module                    extmod/modnetwork.c (the Network
                                      ABC + cyw43/wiznet/... drivers).
                                      DOS NICs are managed via the
                                      packet-driver interface instead;
                                      uc386_net + lwip cover the same
                                      ground at a lower level.

    machine.Pin / I2C / SPI / UART    No DOS-level device model. ISA bus
    /Timer/ADC/DAC/PWM/WDT            access works via machine.mem32, but
                                      the typed peripherals would each
                                      need a driver. Not on the path.

    bluetooth / espnow / btree        Hardware/RTOS-specific upstream
                                      modules. No DOS analogue exists.

    PERSISTENT_CODE_LOAD / .mpy       We don't run mpy-tool, so the
    FROZEN_MPY                        frozen-module symbols would be
                                      undefined externs at link time.
                                      Pure-source .py imports work.

### In progress

    SSH publickey auth                Today only userauth_password is
                                      wired up. session.userauth_publickey
                                      needs real RSA / DH / Ed25519
                                      key-parse in port/libssh2_axtls.c
                                      (currently stubs returning -1).
                                      Tracked in docs/WIP.md.

    Frozen-bytecode loading           Wiring mpy-tool.py + the
                                      mp_frozen_* symbols into the
                                      build would let us ship the
                                      asyncio Python files baked into
                                      the .exe. Mechanical, not
                                      research.

    Wider lwIP surface                IPv6 is off; UDP multicast and
                                      raw sockets are exposed at the
                                      lwIP layer but not surfaced
                                      through PY_SOCKET. Add as
                                      demand appears.

    TCP_NODELAY                       modlwip's setsockopt(TCP_NODELAY)
                                      matches lwIP's TF_NODELAY=0x40
                                      constant, not POSIX TCP_NODELAY=1.
                                      Trivial to fix — open while we
                                      decide whether to break the
                                      lwIP-native users.
