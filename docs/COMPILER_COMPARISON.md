# Building this MicroPython port under other DOS compilers

**This port compiles under uc386 today.** `freedos-micropython
port` produces a real, shippable DOS artifact:

| toolchain | artifact                | size | status |
|-----------|-------------------------|-----:|--------|
| **uc386** | `build/micropython.bin` | ~444 KB at EXTRA_FEATURES + axtls TLS (≈408 KB pre-axtls, ≈296 KB pre-lwIP, ≈169 KB at MINIMUM — see `NOTES.md`) | builds; boots the full Python REPL under `uc386.dos_emu` |

Because it *does* compile under uc386, the next question — the one
the sibling effort tracks for size — is: **how does it look under
the other DOS C toolchains, and is that an option?** This doc
answers honestly and points at the rig that reproduces the numbers.

## TL;DR — run it yourself

```sh
# after `freedos-micropython fetch` && `freedos-micropython build`
FREEDOS_MP_WORK=/path/to/work-dir sh rigs/size-compare/run.sh
```

## What "other compilers" realistically means here

MicroPython's core (`upstream/py/`) is deliberately portable — it
targets bare-metal MCUs, so it does *not* assume a POSIX host the
way git does. That means a foreign compiler can get a long way at
the per-TU level. But a **full** DOS interpreter from DJGPP or Open
Watcom is still its own port: each needs a toolchain-specific
`mpconfigport.h`, a HAL (`mp_hal_stdout_tx_strn` etc.), and the
qstr / frozen-module wiring that this repo currently provides only
for the uc386 + `port/` path.

So the honest, reproducible comparison is the same per-TU compile
triage `NOTES.md` already uses for uc386, run by
`rigs/size-compare/run.sh` over `build/_port_sources.txt`:

- **uc386** — the shipped `micropython.bin` size (real DOS REPL).
- **DJGPP** — N-of-total TUs that compile at `-O2`, plus total
  `.o` bytes. A *linked* DJGPP build would also carry the full
  djgpp C runtime + go32 (cf. the uc386 project's `addons/
  results.md`: even `true` is ~148 KB under DJGPP).
- **Open Watcom V2** — a leaf-TU sample under DOSBox-X-hosted
  `wcc386` (no native macOS Watcom; the DOS binaries run under
  DOSBox-X via uc386's `watcom_dosbox.py`).

Run the rig in your environment to fill in the live numbers; they
are intentionally not hard-coded here so they can't go stale. The
uc386 column is the one that matters most: it is the only toolchain
that produces a working DOS MicroPython today.

## Installing the optional toolchains

- **DJGPP** — `andrewwutw/build-djgpp` v3.4 (gcc 12.2). Linux:
  `djgpp-linux64-gcc1220.tar.bz2`; macOS: `djgpp-osx-gcc1220.tar.bz2`
  (x86_64, runs under Rosetta). Extract to `~/.local/opt/djgpp`.
- **Open Watcom V2** — no native macOS build; its Linux binaries
  don't run under Rosetta. The DOS release asset
  `open-watcom-2_0-c-dos.exe` is a plain self-extracting zip:
  `unzip 'binw/*' 'h/*' 'lib386/*' -d ~/.local/opt/watcom-dos`,
  then `brew install dosbox-x` (or distro `dosbox-x`). The
  DOS-hosted `wcc386.exe`/`wlink.exe` run under DOSBox-X. The
  driver is uc386's `addons/harness/watcom_dosbox.py` — keep a
  uc386 source checkout (sibling `../uc386`) for the Watcom step.

(Identical toolchain layout to the uc386 repo's `docs/INSTALL.md`
and the sibling `freedos_git` port, so one install serves all.)
