# Draft DOSBox-X bug report

Draft for upstream DOSBox-X
(https://github.com/joncampbell123/dosbox-x). Root-caused
2026-05-07; candidate fix verified end-to-end via instrumented
CI.

---

## Title

`CheckQuit()` y/n confirmation hangs forever under `-silent` +
headless SDL: `tinyfd_messageBox` falls through to a
`printf("y/n: ") + _getch()` loop with no stdin source, blocking
clean shutdown until the host kills the process.

## Affected versions

- DOSBox-X `2026.05.02` SDL2 (Homebrew bottle, macOS 25.4.0)
- `apt install dosbox-x` on Ubuntu 22.04 GitHub Actions runner

Both reproduce identically.

## Symptom

A DOS program with this autoexec:

```ini
[autoexec]
mount C /path/with/program
C:
PROGRAM > OUT.TXT
exit
```

…run via `dosbox-x -silent -conf foo.conf` (typical CI / scripted
testing setup) hangs after `PROGRAM` finishes, until the host's
process-level timeout fires `SIGKILL`. The DOSBox-X stderr fills
with thousands of repetitions of:

```
You are currently running a program or game.
Are you sure to quit anyway now?
y/n: 
```

If the program's last writes to `OUT.TXT` happened to use buffered
`fwrite` under DOSBox-X's `LocalFile::Write` (i.e., they were
`INT 21h AH=0x40` calls without a following `INT 21h AH=0x68`
commit), those bytes are also lost — `SIGKILL` skips the
host-stdio flush. Whether they were buffered or not depends on
the program; well-behaved DOS programs that explicitly commit
after writes are unaffected, but C-libc-style `write(2)` wrappers
typically don't commit.

## Reliable reproducer

`MP.EXE` is the uc386-MicroPython binary at
https://github.com/avwohl/uc386 (artifact `mp-rig-artifacts/MP.EXE`
from any successful `mp-rig.yml` run, ~484 KB). Direct CI demo
at
[`dosbox-x-bug.yml`](https://github.com/avwohl/uc386/actions/workflows/dosbox-x-bug.yml).

```ini
# dosbox-x.conf
[cpu]
cputype = pentium
core    = normal
cycles  = max
[dosbox]
memsize = 16

[autoexec]
mount C /path/with/MP.EXE
C:
MP.EXE > MPOUT.TXT
exit
```

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    timeout -k 10 60 dosbox-x -silent -conf dosbox-x.conf
```

DOSBox-X never exits cleanly; it's killed by the host timeout
60 s in. The same MP.EXE under classic DOSBox 0.74-3 or
QEMU+FreeDOS exits cleanly via `exit` after producing the full
657-byte expected `MPOUT.TXT`.

## Root cause

[`CheckQuit()`](https://github.com/joncampbell123/dosbox-x/blob/master/src/gui/sdlmain.cpp#L1340)
in `src/gui/sdlmain.cpp` decides whether to allow a quit. When
the running program isn't `DOSBOX-X`, `COMMAND`, or `4DOS`, it
calls

```cpp
return systemmessagebox(
    "Quit DOSBox-X warning",
    MSG_Get("QUIT_PROGRAM_CONFIRM"),
    "yesno", "question", 1);
```

`systemmessagebox` defers to `tinyfd_messageBox`. With
`-silent` + `SDL_VIDEODRIVER=dummy`, there is no GUI target
available, so `tinyfd_messageBox`'s Linux/macOS fallback
([`tinyfiledialogs.c:2950`](https://github.com/joncampbell123/dosbox-x/blob/master/src/libs/tinyfiledialogs/tinyfiledialogs.c#L2950)
in current master) executes:

```c
do {
    ...
    printf("y/n: ");
    lChar = (char)tolower(_getch());
    printf("\n\n");
} while (lChar != 'y' && lChar != 'n');
```

In a fully-headless context, `_getch()` reads from `/dev/null`
or returns immediately on EOF without ever yielding 'y' or 'n',
so the loop never terminates. DOSBox-X is stuck inside this
loop until the host SIGKILLs it.

The downstream effect — lost buffered redirect-file output — is
itself "expected" for any process that gets SIGKILLed: stdio
buffers are kernel-buffered host state that the kernel discards
on uncatchable signal. But the *reason* DOSBox-X is being
SIGKILLed is the y/n loop, which IS a bug.

## Suggested fix

In `-silent` mode there is by definition no UI to answer the
confirmation; the right behavior is to skip it and allow the
quit. The clean shutdown path then closes all `Files[handle]`
objects, which `fclose`s any redirected output files,
which flushes their stdio buffers — restoring the
normally-expected DOS behavior of "redirected output is on disk
when the shell finishes."

Proposed patch (`src/gui/sdlmain.cpp`):

```c
bool CheckQuit(void) {
#if !defined(HX_DOS)
    /* In -silent mode there is no UI to answer the y/n
     * confirmation; tinyfd_messageBox falls back to
     * printf+_getch which loops forever waiting for stdin.
     * Auto-confirm so DOSBox-X exits cleanly. */
    if (control && control->opt_silent) return true;
    /* ... existing implementation ... */
```

Verified via the
[instrumented CI build](https://github.com/avwohl/uc386/actions/workflows/dosbox-x-instrument.yml):
with this single-hunk patch applied, `dosbox-x -silent -conf …`
exits cleanly when its `[autoexec]` runs `exit`, and the
redirected `MPOUT.TXT` contains the full 657 bytes the program
wrote. Without the patch the same run produces only 481 bytes
(the bytes that happened to be flushed by explicit `AH=68`
commits earlier in the program's startup).

The unified diff is in
[`dosbox-x-fix.patch`](https://github.com/avwohl/uc386/blob/main/addons/gnu/micropython/dosbox-x-rig/dosbox-x-fix.patch)
in this repo.

## Side note: `LocalFile::Write` is buffered on POSIX

Independent of this bug, the non-Windows branch of
`LocalFile::Write` uses `fwrite` (host-stdio buffered) while
the Windows branch uses unbuffered `WriteFile`. So any DOS
program SIGKILLed by anything else (DOSBox-X crash, host OOM
killer, user `kill -9`) on POSIX still loses its last
in-stdio-buffer redirect-file bytes. Programs that explicitly
`INT 21h AH=0x68` (commit) after writes are immune; programs
that rely on real DOS's "writes are immediately durable"
behavior are not. Worth filing separately if anyone wants
POSIX/Windows parity here, but it's not required to fix the
report's symptom.

## Diagnostic infrastructure

Two CI workflows in
https://github.com/avwohl/uc386 reproduce and characterize the
bug end-to-end:

- [`dosbox-x-bug.yml`](https://github.com/avwohl/uc386/blob/main/.github/workflows/dosbox-x-bug.yml):
  runs `MP.EXE` under apt-installed DOSBox-X / classic DOSBox
  0.74-3 / QEMU+FreeDOS, captures output sizes + xxd dumps.
- [`dosbox-x-instrument.yml`](https://github.com/avwohl/uc386/blob/main/.github/workflows/dosbox-x-instrument.yml):
  patches `src/dos/dos.cpp` to log every `INT 21h AH=0x40`
  call's full register state, source linear address, first 16
  bytes of `dos_copybuf`, and pre/post `*amount` from
  `DOS_WriteFile`. Builds DOSBox-X from source with the
  candidate fix patch and runs `MP.EXE`. The captured logs show
  ALL 46 AH=0x40 calls reaching `DOS_WriteFile` with
  `fWritten=1`, `post_towrite==pre_towrite`, identical buffer
  contents — confirming the bug is purely in the shutdown path,
  not in `INT 21h` dispatch.

## Why this matters

Any DOS-program test rig that uses `-silent` + `[autoexec]` for
scripted execution hits this. Specifically every
"build a binary, run it under DOSBox-X, capture stdout via
redirect" CI flow — typical for emulator-based DOS
software-engineering loops. We hit it on a uc386 + MicroPython
+ Open Watcom V2 + PMODE/W test rig where a 484 KB binary
that ran cleanly on classic DOSBox 0.74-3, FreeDOS-on-QEMU,
and the production target (FreeDOS in VMware on Windows) was
silently truncated under DOSBox-X with no error indication.
