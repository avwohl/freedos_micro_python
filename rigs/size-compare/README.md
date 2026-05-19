# size-compare rig

Opt-in size / portability triage of this MicroPython port under the
other DOS C toolchains. Nothing here is required to build or use the
port — it answers "how big, and does it even compile elsewhere?"
with numbers.

## Run

```sh
# from a populated work dir (after `freedos-micropython fetch`
# and `freedos-micropython build`):
FREEDOS_MP_WORK=/path/to/work-dir sh rigs/size-compare/run.sh
```

Reports, over the exact source list this port feeds uc386
(`build/_port_sources.txt`):

- **uc386** — the shipped `build/micropython.bin` size (the real
  DOS REPL artifact).
- **DJGPP** — per-TU compile triage at `-O2` (how many of the N
  TUs gcc-12→go32 accepts, plus total `.o` size). MicroPython's
  core `upstream/py/` is written to be portable, so DJGPP gets
  meaningfully far; the DOS HAL / generated wiring do not.
- **Open Watcom V2** — a leaf-TU sample compiled by the DOS-hosted
  `wcc386` under DOSBox-X (reuses uc386's `watcom_dosbox.py`), so
  the period reference is *measured*, not assumed.

Both foreign toolchains are optional; a missing one is skipped with
a note. A *full* DJGPP/Watcom DOS build of MicroPython is a separate
port (each needs its own `mpconfigport.h` + HAL + qstr wiring).

See [`../../docs/COMPILER_COMPARISON.md`](../../docs/COMPILER_COMPARISON.md)
for the honest write-up and toolchain install instructions.
