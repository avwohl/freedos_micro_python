# freedos_micro_python

> **AI — no code written by a primate.**

[MicroPython](https://github.com/micropython/micropython) port for
**FreeDOS / i386**, built end-to-end through the
[uc386](https://pypi.org/project/uc386/) C23 compiler. Produces a
runnable flat-binary or PMODE/W `.exe` with a fully-functional
Python REPL — arithmetic, control flow, classes, list comprehensions,
exception handling, and ~25 named builtins all work.

**📖 User manual:** <https://avwohl.github.io/freedos_micro_python/>

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
- See [`NOTES.md`](https://github.com/avwohl/freedos_micro_python/blob/main/NOTES.md) for the full per-slice development log

## Install on FreeDOS

If you just want to *run* MicroPython on a DOS machine, you don't need
any of the build tooling below. The port ships as a standard FreeDOS
package — copy `mpython.zip` to the machine and:

```
FDNPKG install MPYTHON.ZIP
```

That puts `MP.EXE` in `C:\DEVEL\MPYTHON`, registers the package, and
puts `MP` on your `%PATH%`. Then `MP` starts the REPL and
`MP SCRIPT.PY` runs a script.

`FDINST install MPYTHON.ZIP` does the same on pre-386 machines and
needs no network. See
[`docs/FREEDOS_PACKAGING.md`](https://github.com/avwohl/freedos_micro_python/blob/main/docs/FREEDOS_PACKAGING.md) for the
package layout, how to serve it as an FDNPKG repository, and where it
stands with the official FreeDOS repository.

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

The output is `./build/micropython.bin`, a flat i386 DOS binary.
[`docs/BUILDING.md`](https://github.com/avwohl/freedos_micro_python/blob/main/docs/BUILDING.md) covers running the binary under uc386's
emulator, producing a real DOS `.exe` with uc386's `addons/harness/exe.py`,
running the tests, and the source layout.

## Bundled networking utilities

The port ships three pure-MicroPython programs, `wget.py`, `scp.py` and
`sftp.py`, in [`examples/`](https://github.com/avwohl/freedos_micro_python/blob/main/examples/). The three programs double as regression
tests and as usable standalone tools. See
[`docs/bundled-utilities.md`](https://github.com/avwohl/freedos_micro_python/blob/main/docs/bundled-utilities.md).

## Documentation

- [User manual](https://avwohl.github.io/freedos_micro_python/) - the full manual, also in [`docs/`](https://github.com/avwohl/freedos_micro_python/blob/main/docs/index.md)
- [`docs/feature-matrix.md`](https://github.com/avwohl/freedos_micro_python/blob/main/docs/feature-matrix.md) - MicroPython features enabled, not implemented, and in progress
- [`docs/bundled-utilities.md`](https://github.com/avwohl/freedos_micro_python/blob/main/docs/bundled-utilities.md) - running programs, and the wget / scp / sftp tools
- [`docs/BUILDING.md`](https://github.com/avwohl/freedos_micro_python/blob/main/docs/BUILDING.md) - build tooling, quick start, testing, source layout
- [`docs/FREEDOS_PACKAGING.md`](https://github.com/avwohl/freedos_micro_python/blob/main/docs/FREEDOS_PACKAGING.md) - the FreeDOS package and FDNPKG repository
- [`docs/TESTS.md`](https://github.com/avwohl/freedos_micro_python/blob/main/docs/TESTS.md) - catalog of tests and rigs
- [`docs/WIP.md`](https://github.com/avwohl/freedos_micro_python/blob/main/docs/WIP.md) - work in progress and known issues
- [`docs/THIRD_PARTY.md`](https://github.com/avwohl/freedos_micro_python/blob/main/docs/THIRD_PARTY.md) - third-party projects and licenses
- [`NOTES.md`](https://github.com/avwohl/freedos_micro_python/blob/main/NOTES.md) - per-slice development log

## A debt to FreeDOS

This project targets [FreeDOS](https://www.freedos.org/) and would have been
impossible without the FreeDOS source tree to read. The `release/` directory
ships a copy of the FreeDOS sources the project leaned on. The full statement is
in [`docs/credits.md`](https://github.com/avwohl/freedos_micro_python/blob/main/docs/credits.md#a-debt-to-freedos).

## License

[MIT](https://github.com/avwohl/freedos_micro_python/blob/main/LICENSE), matching upstream MicroPython. The integration glue
(scripts, port files, CLI, tests) is what's covered here. Third-party
sources fetched by `build_port.sh` (MicroPython, axtls, lwIP,
libssh2, TweetNaCl, crypto-algorithms) retain their own licenses;
the FreeDOS sources in `release/` retain GPLv2 / their own
per-project licenses. The full catalog with attributions is in
[`docs/THIRD_PARTY.md`](https://github.com/avwohl/freedos_micro_python/blob/main/docs/THIRD_PARTY.md).

## Related projects

- [FreeDOS](https://www.freedos.org/) — The target operating system. This port runs on FreeDOS on i386.
- [uc386](https://github.com/avwohl/uc386) — C23 compiler for the i386 processor and MS-DOS. It builds this port and hosts the `dos_emu` test harness.
- [uc_core](https://github.com/avwohl/uc_core) — Shared C23 frontend and AST optimizer that the uc386 compiler and its Z80 sibling uc80 both use.
- [MicroPython](https://github.com/micropython/micropython) — The upstream project. This repository is its port for FreeDOS on i386.
