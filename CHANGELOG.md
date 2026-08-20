# Changelog

Notable changes to freedos_micro_python. Releases before 0.2.4 are described on the
[GitHub releases page](https://github.com/avwohl/freedos_micro_python/releases).

## 0.2.4 — 2026-08-20

No change to this project's own source since 0.2.3. This release raises the
`uc386` floor to `>=0.2.5` so that a fresh `pip install` gets a compiler that
in turn resolves `upeep386` 0.2.1 and `uc_core` 0.4.1.

### Changed

- `uc386` floor raised from `>=0.2.1` to `>=0.2.5`. The floor had been left at
  0.2.1 across four uc386 releases, so pip was free to resolve an old uc386
  that pulled `upeep386` 0.2.0. That peephole pass collapses an 80-bit
  `fld`/`fmulp` pair into an `fmul tword` NASM will not encode, which breaks
  the assembly step for anything using the printf float path, and it treats
  the caller-saved registers as dead across a `call`, which silently deletes
  the argument setup for the register-convention helpers in the bundled libc.
