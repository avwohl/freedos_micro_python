---
title: Credits + third-party licenses
---

# Credits and third-party licenses

This manual stands on the shoulders of giants. So does the binary
it documents.

## Documentation sources

Substantial portions of this manual — language summary, builtin
function signatures, standard-library module surfaces — are
adapted from the official documentation of the following projects:

- [**MicroPython documentation**](https://docs.micropython.org/),
  MIT-licensed, Copyright © 2014-2024 Damien P. George, Paul
  Sokolovsky, and contributors. Most of the per-module reference
  pages (`sys`, `os`, `time`, `math`, `random`, `struct`,
  `hashlib`, `binascii`, `re`, `json`, `heapq`, `collections`,
  `io`, `gc`, `errno`, `select`, `asyncio`, `socket`, `ssl`,
  `deflate`, `uctypes`, `machine`) follow MicroPython's structure
  and signatures. Examples are mostly DOS-adapted by this port.

- [**Python language documentation**](https://docs.python.org/3/),
  PSF-licensed, Copyright © 2001-2024 Python Software Foundation.
  Language summary on the [language](language.md) page draws on
  the
  [tutorial](https://docs.python.org/3/tutorial/) and
  [reference](https://docs.python.org/3/reference/) chapters.

- [**libssh2 documentation**](https://www.libssh2.org/docs.html),
  BSD-3-Clause, Copyright © 2004-2024 The libssh2 project.
  Terminology in [`_ssh`](library/ssh.md).

Material original to this manual — DOS-specific quirks, the
networking-internals page, the troubleshooting guide, this
credits page — is © 2026 Aaron Wohl and the freedos_micro_python
contributors, MIT-licensed alongside the rest of the project.

## Software bundled in MP.EXE

The binary you run is the integration of several upstream
projects compiled together. The authoritative catalog —
versions, licenses, modifications, source URLs — is in
[`docs/THIRD_PARTY.md`](THIRD_PARTY.md) in the source tree.
Summary:

| Component               | License        | What it does                          |
|-------------------------|----------------|---------------------------------------|
| MicroPython             | MIT            | Python interpreter + standard library  |
| axtls                   | BSD-3-Clause   | TLS (`ssl`) + hashes / AES used by `_ssh` |
| libssh2                 | BSD-3-Clause   | SSH/SCP/SFTP client protocol           |
| TweetNaCl               | Public Domain  | Curve25519 KEX + Ed25519 host-key verify |
| lwIP                    | BSD-3-Clause   | TCP/IP stack                           |
| crypto-algorithms       | Public Domain  | SHA-256 (kept for `hashlib`'s API)     |
| PMODE/W                 | Free for use   | 32-bit DOS extender (stub baked into the .exe) |
| uc386                   | MIT            | C23 compiler that built it all         |

Plus the port's own glue — `src/freedos_micro_python/port/*.c`
and the build/CLI scripts — under [MIT](https://github.com/avwohl/freedos_micro_python/blob/main/LICENSE).

## A debt to FreeDOS

Targeting FreeDOS would have been impossible without the FreeDOS
source tree to read when debugging PMODE/W's INT 21h reflection,
the DOS packet-driver interface, the FAT write path, and a
handful of NLS / RTC quirks. The `release/` directory in the
repository ships a copy of the FreeDOS sources we leaned on; see
the project [README](https://github.com/avwohl/freedos_micro_python#a-debt-to-freedos)
for the full statement.

[FreeDOS](https://www.freedos.org/) is GPLv2-licensed and the
work of many contributors over many years. We use it as a target,
not modify it.

## Test fixtures

The bundled SSH / TLS rigs use:

- [paramiko](https://www.paramiko.org/) — LGPL-2.1, as the SSH
  server fixture
- [OpenSSL](https://www.openssl.org/) — Apache-2.0, for generating
  test certs in the TLS rig
- [QEMU](https://www.qemu.org/), [DOSBox-X](https://dosbox-x.com/),
  [dosiz](https://github.com/avwohl/dosiz) — all GPLv2/3 — as the
  DOS emulators

None of these are included in the .exe; they're host-side test
dependencies.

## Thank you

Open-source software has always been a debt-to-strangers economy.
This manual, and the port it documents, exists because of the
work of every contributor to every project above. If you find a
bug here, file it at
<https://github.com/avwohl/freedos_micro_python/issues>; if you
improve it, we'd love the PR.
