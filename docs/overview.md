---
title: What is MicroPython?
---

# What is MicroPython?

MicroPython is a real implementation of the Python 3 language that
runs in tens-to-hundreds of kilobytes of memory. The same code base
runs on microcontrollers (ESP32, RP2040, STM32, …), in WebAssembly,
on Unix, and — with this port — on FreeDOS / i386.

When you type at the `>>>` prompt, you are running Python: the
bytecode compiler, the garbage collector, the type system, and the
standard-library subset are all the genuine MicroPython
implementations. There is no translation layer to "DOS BASIC" or
similar.

## What is the same as desktop Python

- **Language.** Comprehensions, generators, classes, multiple
  inheritance, decorators, exceptions, context managers, `with`,
  `yield from`, f-strings, keyword-only arguments, `*args` /
  `**kwargs`, `async` / `await`, …
- **Numerics.** `int` is arbitrary precision (heap-allocated long
  ints), `float` is double-precision (x87 FPU), `complex` is not
  built in.
- **Strings + bytes.** `str` is unicode (UTF-8 encoded internally),
  `bytes` and `bytearray` are 8-bit. `b'...'` literals, `f'...'`,
  slicing, encoding, `.format()`, all work.
- **Standard library.** `os`, `sys`, `time`, `io`, `re`, `json`,
  `struct`, `hashlib`, `binascii`, `random`, `math`, `select`,
  `collections`, `heapq`, `errno`, `gc`, `asyncio`, …
- **Networking.** `socket` (BSD-style, TCP/UDP/DNS), `ssl` (TLS
  client + server with `CERT_REQUIRED`).

## What is different

The headline differences from desktop CPython, all driven either by
MicroPython's design or by DOS being a 32-bit single-process OS with
no threads and no virtual memory:

- **No threads.** `_thread` is off; DOS is single-threaded and
  pre-emptive threads would mislead. `asyncio` runs fine as a
  cooperative scheduler.
- **Some types are simpler.** `dict` preserves insertion order
  (like CPython 3.7+) but does not implement views. `set`
  iteration order is arbitrary.
- **No `__slots__`, no `__class_getitem__`, no PEP 695 generics.**
  These are mostly type-system bells you don't notice unless you're
  writing library code that targets every CPython version.
- **No `weakref`, no `cmath`.** Skipped because no current code
  needs them on DOS; could be enabled if a use case appears.
- **No VFS.** MicroPython upstream supports mounting multiple
  filesystems through `MICROPY_VFS`. We have a single flat
  file-import path through INT 21h instead — sufficient for the
  DOS FAT filesystem the program sees.
- **8.3 filenames.** DOS FAT does not have long names. `open('CLIENT.KEY')`
  works; `open('client.key')` only works because DOS case-folds.
- **No hardware abstraction (`machine.Pin`, `I2C`, `SPI`, …).**
  DOS has no pin model. ISA bus access works through
  [`machine.mem32`](library/machine.md), but typed peripherals
  would each need a driver.

## What is unique to this port

Things that the FreeDOS port exposes which you won't find on a
microcontroller MicroPython:

- **`open()` opens real DOS files** via INT 21h AH=3D/3F/40. Long
  reads stream chunk-by-chunk through the DPMI bounce buffer; no
  `tempfile` is required.
- **A real packet driver stack.** The port talks to whatever Crynwr
  packet driver is loaded at INT 60h (NE2000, PCI realtek, the
  dosbox/dosiz synthetic driver). On top of that runs lwIP, and on
  top of lwIP runs MicroPython's `socket` module.
- **TLS via axtls.** Full handshake including `CERT_REQUIRED` with
  a user-supplied CA bundle.
- **SSH/SFTP/SCP via libssh2.** Curve25519 KEX + Ed25519 host-key
  verify (TweetNaCl) + AES-256-CTR + HMAC-SHA-256 (axtls). Password
  and Ed25519 publickey auth.

## A note on memory

MicroPython on this port runs with a 1 MB Python heap (the GC
manages it) plus PMODE/W's PM heap for everything outside the GC
(libssh2 internal buffers, axtls cert chains, lwIP packet buffers).
On a 16 MB DOS machine the port is comfortable; on the 4 MB minimum
it is tight but works for small programs.

`gc.collect()` works and returns the same shape of result it does
in CPython (`None`; the freed-bytes value is available through
`gc.mem_free()` / `gc.mem_alloc()`).

---

Next: [Getting started](getting-started.md) — running `MP.EXE` on
your DOS system.
