---
title: FreeDOS MicroPython Manual
---

# FreeDOS MicroPython Manual

Python 3 for FreeDOS / i386 — a fully-functional interactive Python
REPL, a real `import`-driven module system, and an honest networking
stack (TCP / TLS / SSH / SCP / SFTP) that runs on a 1990s-era PC.

```
MicroPython uc386-triage on 2026-05-01; uc386-dos with i386
Type "help()" for more information.
>>> def fib(n):
...     if n < 2: return n
...     return fib(n-1) + fib(n-2)
...
>>> print([fib(i) for i in range(10)])
[0, 1, 1, 2, 3, 5, 8, 13, 21, 34]
>>>
```

This site is the user manual. The source repository lives at
<https://github.com/avwohl/freedos_micro_python>.

If you want the developer/build-history notes instead, see
[`WIP.md`](WIP.md) in the same directory.

---

## Where to start

If you've never run MicroPython on DOS before, read these in order:

1. [What is MicroPython?](overview.md) — one-page tour of the language
   subset, what's different from desktop CPython, and where DOS
   constraints show through.
2. [Getting started](getting-started.md) — getting `MP.EXE` onto a
   FreeDOS system (real hardware, QEMU, DOSBox-X, dosiz) and running
   your first script.
3. [Using the REPL](repl.md) — the interactive prompt, paste mode,
   keyboard shortcuts, how to run scripts and `import` your own
   `.py` files.

## Manual contents

### Language

- [The Python language, minimum viable summary](language.md)
- [Built-in functions](builtins.md) — `print`, `len`, `range`,
  `open`, `eval`, ...
- [Built-in types](types.md) — `int`, `float`, `str`, `bytes`,
  `bytearray`, `list`, `tuple`, `dict`, `set`, `frozenset`,
  `memoryview`, `range`

### Standard library

Core:
- [`sys`](library/sys.md), [`os`](library/os.md), [`time`](library/time.md),
  [`io`](library/io.md), [`gc`](library/gc.md),
  [`errno`](library/errno.md)

Numerics + data:
- [`math`](library/math.md), [`random`](library/random.md),
  [`struct`](library/struct.md), [`binascii`](library/binascii.md),
  [`hashlib`](library/hashlib.md), [`deflate`](library/deflate.md),
  [`re`](library/re.md), [`json`](library/json.md),
  [`collections`](library/collections.md), [`heapq`](library/heapq.md),
  [`uctypes`](library/uctypes.md)

Networking + crypto:
- [`socket`](library/socket.md), [`ssl`](library/ssl.md),
  [`select`](library/select.md), [`_ssh`](library/ssh.md),
  [`lwip`](library/lwip.md), [`uc386_net`](library/uc386_net.md)

Concurrency:
- [`asyncio`](library/asyncio.md) — cooperative event loop, no
  pre-emptive threads.

### DOS-specific

- [DOS file I/O, paths, 8.3 names, line endings](dos-features.md)
- [`machine`](library/machine.md) — direct memory access at PM
  linear addresses
- [`dosint21`](library/dosint21.md) — raw INT 21h dispatch through
  the DPMI 0x0301 thunk
- [Packet driver INT 60h, lwIP poll loop](networking-internals.md)

### Bundled tools

The port ships three pure-MicroPython programs that run end-to-end
on DOS — drop them into a floppy image and they work as standalone
utilities.

- [`wget.py`](tools/wget.md) — HTTPS streaming downloader
- [`scp.py`](tools/scp.md) — SCP client
- [`sftp.py`](tools/sftp.md) — interactive `sftp(1)`-style shell

### Practical

- [Cookbook](cookbook.md) — common tasks: read a file, talk to a
  TCP server, hash a download, run something every N seconds, …
- [Troubleshooting](troubleshooting.md) — what to do when something
  doesn't work
- [Credits + third-party licenses](credits.md)

---

## What works, what doesn't

The feature matrix at the bottom of the
[repo README](https://github.com/avwohl/freedos_micro_python#micropython-feature-matrix)
is the authoritative answer.

The short version: **the language is real Python 3.** Comprehensions,
generators, classes, exceptions, decorators, `yield from`, f-strings,
keyword-only args, `*args` / `**kwargs`, `async`/`await` —
everything you'd expect from a modern Python.

What's missing is mostly hardware-specific or RTOS-specific upstream
modules (`_thread`, `network.WLAN`, `machine.Pin/I2C/SPI`, `bluetooth`,
`btree`, …) that have no DOS analogue, plus a few we just haven't
needed yet (`cmath`, `weakref`).

---

## Credits

Big chunks of this manual are adapted from the [official MicroPython
documentation](https://docs.micropython.org/) (MIT-licensed,
Copyright © 2014-2024 Damien P. George and contributors) and the
[CPython documentation](https://docs.python.org/3/) (PSF-licensed,
Copyright © 2001-2024 Python Software Foundation). Where a
description, table, or example came over largely verbatim we've
left it in place and credited the source on the page footer; where
we needed to adapt for DOS, the wording is ours.

The complete list of third-party software bundled or fetched at
build time — MicroPython itself, axtls (TLS), libssh2 (SSH),
TweetNaCl (Curve25519 / Ed25519), lwIP (TCP/IP), crypto-algorithms
(SHA), PMODE/W (DOS extender), FreeDOS — is in
[`docs/THIRD_PARTY.md`](THIRD_PARTY.md) in the repo.
