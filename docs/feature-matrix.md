---
title: MicroPython feature matrix
---

# MicroPython feature matrix

Settings come from
[`src/freedos_micro_python/port/mpconfigport.h`](https://github.com/avwohl/freedos_micro_python/blob/main/src/freedos_micro_python/port/mpconfigport.h).
The port runs at `MICROPY_CONFIG_ROM_LEVEL = EXTRA_FEATURES`, the
richest preset upstream ships.

## Enabled

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

## Not implemented

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

## In progress

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
