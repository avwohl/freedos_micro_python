# DOSBox-X PMODE/W INT 21h AH=0x40 caller-state-dependent bug

Reproducer scaffolding for the upstream
[joncampbell123/dosbox-x](https://github.com/joncampbell123/dosbox-x)
bug. The directory contains:

| File              | Role                                                |
|-------------------|-----------------------------------------------------|
| `pmwbug.asm`      | Standalone NASM repro (does NOT trigger the bug —   |
|                   | included as a starting point for further bisection) |
| `pmwbug_user.asm` | MP.EXE-shaped repro for use with exe.py bridge      |
|                   | (does NOT trigger either; same prolog/structure)    |
| `build.sh`        | nasm + wlink + PMODE/W stub for `pmwbug.asm`        |

The canonical reproducer that DOES trigger the bug is `MP.EXE`,
the uc386-MicroPython binary built by the parent workflow
(`mp-rig.yml`). Until we find what specifically about MP.EXE
is necessary to fire the bug, the upstream report has to use
that 484 KB binary as the artifact.

## Bisect log — what triggers the bug

Tested on macOS Darwin 25.4.0 with DOSBox-X 2026.05.02 SDL2
(Homebrew). All bisects use the same MP.EXE binary.

| # | autoexec content                                            | Output |
|---|-------------------------------------------------------------|--------|
| 1 | (default DOSBox-X conf, full rig with NE2000+SLIRP)         | BUG    |
| 2 | minimal conf, autoexec: `MP.EXE > MPOUT.TXT / exit`         | BUG    |
| 3 | minimal conf, autoexec: `echo X / MP.EXE > X / echo Y / exit` | NO BUG |
| 4 | rig conf via `call autoexec.bat`                            | BUG    |
| 5 | minimal conf, autoexec: `echo X / MP.EXE > X / exit`        | BUG    |
| 6 | minimal conf, autoexec: `MP.EXE > X / dir / exit`           | BUG    |
| 7 | minimal conf, autoexec: `MP.EXE > X / echo Y / exit`        | BUG    |
| 8 | minimal conf, autoexec: `MP.EXE > X` (no exit)              | NO BUG |

So the bug fires when:

- MP.EXE is invoked via DOSBox-X's `[autoexec]` section (NOT
  the `-c "MP.EXE > X"` command-line option, which doesn't
  trigger), AND
- `exit` follows MP.EXE — either immediately, or with a single
  intervening command (other than `echo` placed in just the
  right pattern).

It does NOT fire when:

- MP.EXE is invoked via `-c` flags
- The autoexec has no `exit` (so DOS sits at the prompt after MP)
- The autoexec has multiple commands surrounding MP.EXE

This is consistent with a state-dependent emulation defect
where the COMMAND.COM stack contents (which depend on the
autoexec command queue) overlap something PMODE/W relies on
during INT 21h reflection. See
`../DOSBOX-X-BUG-REPORT.md` for the upstream draft and the
agent-investigated suspect locations in DOSBox-X source.

## Symptom

`MPOUT.TXT` is exactly 481 bytes, ending at
`[bridge-pre-call-main]\n`. The next line should be
`[mp-main-entered]\n` produced by `_main → _write` (a normal
cdecl call to `INT 21h AH=0x40 BX=1`). That call writes zero
bytes to the redirected file under the bug-triggering config,
even though identical-args `INT 21h AH=0x40` calls from the
bridge stub (low CS:EIP) immediately preceding it work
correctly.

Same MP.EXE under classic DOSBox 0.74-3 or QEMU+FreeDOS prints
the full REPL banner.

## Failed-to-reproduce repros (not yet sufficient)

`pmwbug.asm` — single _start with two `INT 21h AH=0x40` call
sites (low CS:EIP and high CS:EIP after 768 KB of NOP padding)
sharing one `_writer` helper. No bug under DOSBox-X with
either bug-triggering or non-triggering autoexec layout.

`pmwbug_user.asm` — depth-3 call chain (`_start → _main → _write`)
with `_main` and `_write` byte-for-byte matched to MP.EXE's
emitted prologs, padded to ~2 MB into the code segment, plus
1 MB BSS. Built via the exe.py bridge (same 2-obj LE shape as
MP.EXE). No bug.

So the trigger requires more than caller CS:EIP and more than
"large code object". Outstanding hypotheses:

- MicroPython's `mp_init` does FPU operations before `_main`'s
  marker write — FPU state may matter.
- MP.EXE has ~196 TUs worth of cross-obj fixups; relocation
  table size or pattern may matter.
- Stack-canary or specific page-table layout of the larger
  binary may be required.

The minimal-repro hunt is on hold pending source-side
instrumentation of DOSBox-X (see DOSBOX-X-BUG-REPORT.md).
