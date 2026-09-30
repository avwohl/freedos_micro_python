# Building and testing the port

Developer notes: install the build tooling, build `micropython.bin`, run the tests, and find your way around the source tree. The [README](../README.md) has the short version.

## Install the build tooling

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
- `rigs/fdpkg-rig/` — installs the FreeDOS package with the real
  FreeDOS installer on a real FreeDOS kernel under QEMU
- `release/mkfdpkg.py` — builds the FreeDOS package and a drop-in
  FDNPKG repository from a built `MP.EXE`
