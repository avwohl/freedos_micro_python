---
title: Getting started
---

# Getting started

This page gets `MP.EXE` running on a FreeDOS system. Pick the flow
that matches your setup.

## 1. Obtaining MP.EXE

You have two choices: download a prebuilt `MP.EXE` from the
[Releases page](https://github.com/avwohl/freedos_micro_python/releases),
or build one yourself. The release binary is ~660 KB and runs out
of the box on FreeDOS / DOSBox-X / qemu+FreeDOS / dosiz.

### Building from source

You need:

- A Unix-y shell (macOS or Linux)
- `git`, `python3` (≥ 3.10), `pip`, `nasm`

```bash
pip install freedos_micro_python    # pulls in uc386 as a dep
mkdir mp-build && cd mp-build
freedos-micropython fetch           # ~30 s — clones upstream MP + axtls + libssh2 + lwIP + TweetNaCl
freedos-micropython build           # ~3 min — generates qstr tables, runs the triage pass
freedos-micropython port            # ~30 min — multi-TU compile via uc386, then NASM
# → build/micropython.bin            (flat binary, runs in uc386's emulator)

# Wrap to a real DOS .exe (PMODE/W stub, +12 KB):
python -m addons.harness.exe build/micropython.asm -o MP.EXE
```

The 30-minute `port` step is doing a from-scratch C compile of
~242 sources (MicroPython core + axtls + libssh2 + lwIP +
TweetNaCl + crypto-algorithms + our port glue) through uc386's
C23 frontend. There's no incremental rebuild — it's all or nothing.

## 2. Getting MP.EXE onto your DOS system

### Real DOS hardware

Copy `MP.EXE` onto whatever transfer medium your machine accepts —
floppy, ZIP disk, CF card, network share. No memory configuration
required: PMODE/W (linked into the .exe) brings up its own 32-bit
PM environment, allocates its DPMI heap from the upper-memory
arena, and runs.

If you want networking, you need a Crynwr-compatible packet driver
loaded at INT 60h (NE2000.COM, PCNTPK.COM, RTSPKT.COM, …) *before*
launching `MP.EXE`. Check
<https://www.crynwr.com/drivers/> for the canonical drivers.

### qemu + FreeDOS

```bash
# 1. Build a FAT12 image with MP.EXE + NE2000.COM:
cp freedos-1.4MB.img test.img
mcopy -i test.img MP.EXE      ::MP.EXE
mcopy -i test.img NE2000.COM  ::NE2000.COM

# 2. Boot:
qemu-system-i386 -fda test.img -boot a -m 16 \
    -netdev user,id=net0 \
    -device ne2k_isa,netdev=net0,iobase=0x300,irq=9 \
    -serial stdio
```

The `-serial stdio` redirect lets you `CTTY COM1` from inside DOS
and drive the REPL over your host terminal. The `rigs/ssh-rig/`
in the repository is exactly this pattern wrapped as a script.

### DOSBox-X

DOSBox-X comes with built-in NE2000 emulation and a SLIRP backend
that NATs the guest behind the host's IP. Drop `MP.EXE` and
`NE2000.COM` into the configured `mount C` directory, set the
DOSBox-X config to:

```ini
[ne2000]
ne2000          = true
nicbase         = 0x300
nicirq          = 9
backend         = slirp

[ethernet, slirp]
ipv4_network    = 10.0.2.0
ipv4_netmask    = 255.255.255.0
ipv4_host       = 10.0.2.2
ipv4_dhcp_start = 10.0.2.15

[autoexec]
NE2000 0x60 9 0x300
MP.EXE
```

and `dosbox-x -conf yourconf.conf`.

### dosiz

[dosiz](https://github.com/avwohl/dosiz) is an MS-DOS emulator that
links dosbox-staging's CPU emulator in-process and traps INT 21h
to C++ host implementations. As of dosiz commit
[`237339d`](https://github.com/avwohl/dosiz/commit/237339d) it
runs MicroPython's PMODE/W-wrapped MP.EXE end-to-end:

```bash
DOSIZ_BYPASS_EXTENDER=1 dosiz MP.EXE
```

The env var tells dosiz to load the LE payload directly via its
own DPMI host instead of letting PMODE/W's internal DPMI take over.

## 3. Your first script

The simplest possible script — save as `HELLO.PY` next to `MP.EXE`:

```python
print('hello from DOS')
print(2 + 2)
import sys
print('python:', sys.implementation.name)
```

Run it by redirecting it into `MP.EXE`'s stdin:

```
A:\> MP.EXE < HELLO.PY
MicroPython uc386-triage on 2026-05-01; uc386-dos with i386
Type "help()" for more information.
>>> hello from DOS
4
python: micropython
```

Or run interactively:

```
A:\> MP.EXE
MicroPython uc386-triage on 2026-05-01; uc386-dos with i386
Type "help()" for more information.
>>> import HELLO
hello from DOS
4
python: micropython
>>>
```

To exit the REPL, type `import sys; sys.exit()` or press Ctrl-D.

## 4. Next steps

- [Using the REPL](repl.md) — paste mode, history (no), Ctrl-C
  semantics
- [Cookbook](cookbook.md) — common recipes
- [Bundled tools](index.md#bundled-tools) — `WGET.PY`, `SCP.PY`,
  `SFTP.PY`

---

*Credit: build-flow text is original to this port. Run-on-DOSBox
instructions adapted from the project's own
[`rigs/dosbox-x-rig/README.md`](https://github.com/avwohl/freedos_micro_python/tree/main/rigs/dosbox-x-rig).*
