# DOSBox-X test rig for the MicroPython binary

Boots DOSBox-X with NE2000 + SLIRP networking and runs the
`MP.EXE`-bound MicroPython under it. Validates the binary's full
real-DOS path:

- IVT scan via DPMI INT 31h fn 0x0200 finds the Crynwr driver
- `pktdrv_init` allocates a real-mode callback via DPMI fn 0x0303
- AH=02 access_type registers the trampoline
- AH=06 fetches the NIC's MAC
- DHCP from SLIRP gives `10.0.2.15` (same network layout the
  dos_emu sim defaults to, so `eth_status()` should be identical
  in both environments)

## What the rig contains

| File             | Role                                                  |
|------------------|-------------------------------------------------------|
| `dosbox-x.conf`  | DOSBox-X config: NE2000 at I/O 0x300 IRQ 9 + SLIRP    |
| `autoexec.bat`   | Loads `NE2000.COM` at INT 0x60, runs baseline + `MP.EXE` |
| `fetch.sh`       | Downloads `NE2000.COM` from archive.org (idempotent)  |
| `run.sh`         | Launches DOSBox-X with the config                     |
| `NE2000.COM`     | Crynwr 11.4.3 packet driver (fetched on first run)    |
| `ECHOTEST.EXE`   | Printf-only baseline (built from `addons/gnu/echo`)   |
| `MP.EXE`         | Watcom-bound MicroPython (must be built on Linux)     |
| `RIG.LOG`        | DOSBox-X session output (created on each run)         |

## Quick check (no MP.EXE needed)

```sh
brew install dosbox-x          # macOS
./fetch.sh                     # downloads NE2000.COM
./run.sh                       # boots DOSBox-X, autoexec runs

cat RIG.LOG
# Expected:
#   --- DOSBox-X test rig ---
#   Packet driver for NE2000, version 11.4.3
#   Packet driver software interrupt is 0x60 (96)
#   Interrupt number 0x9 (9)
#   I/O port 0x300 (768)
#   My Ethernet address is XX:XX:XX:XX:XX:XX
#   MP.EXE not found in C:
#   --- Test rig finished ---
```

That confirms DOSBox-X starts cleanly, NE2000.COM loads, and the
packet driver is installed at INT 0x60 — exactly where
`pktdrv_uc386dos.c::pktdrv_detect()` scans for the "PKT DRVR"
signature.

## Full run (with MP.EXE)

`MP.EXE` is the Watcom-linked MicroPython binary produced by
`addons/harness/exe.py`. Open Watcom doesn't have a macOS build,
so the `.exe` step has to happen on Linux:

```sh
# On a Linux dev box (or a CI runner):
cd /path/to/uc386
python -m addons.harness.exe \
    addons/gnu/micropython/build/micropython.bin \
    -o addons/gnu/micropython/dosbox-x-rig/MP.EXE
```

Then copy the `.exe` back to the Mac and re-run `./run.sh`. The
log will continue past the "MP.EXE not found" branch into the
MicroPython REPL banner + DHCP exchange.

## How DOSBox-X's SLIRP matches dos_emu_netsim

Both pin the same network layout (it's the QEMU/SLIRP convention):

| Field         | Address          |
|---------------|------------------|
| Network       | 10.0.2.0/24      |
| Host gateway  | 10.0.2.2         |
| DNS server    | 10.0.2.3         |
| Guest (DHCP)  | 10.0.2.15        |

So a binary that DHCPs to 10.0.2.15 against the dos_emu sim should
DHCP to the same address against DOSBox-X — useful for "same code,
two environments" sanity.

## Known limitation: MP.EXE runtime under PMODE/W

The `.github/workflows/mp-rig.yml` CI run currently gates on:

- `build_port.sh` produces `micropython.bin`
- `addons/harness/exe.py` binds `MP.EXE` cleanly
- DOSBox-X boots the rig and `NE2000.COM` reports its install
  banner with INT 0x60

What's *not* yet validated end-to-end: MP.EXE's actual runtime
output. With cycles=max + 180s timeout under DOSBox-X 2024.03,
MP.EXE produces no observable bytes on stdout — neither the REPL
banner nor any post-`eth_init` print. The autoexec markers
(`before-mp`, `after-mp`) confirm execution stops between them,
but whether MP.EXE crashes silently in startup or hangs on stdin
parsing isn't pinned down yet.

### Bisect — what we know (as of 2026-05-07)

A series of progressive triage iterations under the rig narrowed
the failure to a specific layer:

1. **Bridge stub runs cleanly.** Markers `[bridge-entered]` →
   `[bridge-argv-done]` → `[bridge-pre-jump]` → `[bridge-post-fpu]`
   → `[bridge-pre-bss-zero]` → `[bridge-post-bss-zero]` →
   `[bridge-diag-stub]` → `[bridge-post-diag]` →
   `[bridge-pre-call-main]` all print under MP.EXE.
2. **Codegen `_start`'s rep stosb on a 280 KB BSS range silently
   aborts** the program under PMODE/W — `[bridge-pre-bss-zero]`
   prints but `[bridge-post-bss-zero]` doesn't when it's enabled.
   Workaround: skip the loop entirely (PMODE/W's loader already
   zero-fills BSS at load time per the LE bss_size header).
3. **In-bridge diag stub `_diag_main()` works.** `[bridge-diag-stub]`
   and `[bridge-post-diag]` both print. So "call C-style function
   with enter-prolog → libc INT-21h marker → ret" plumbing works.
4. **`call _main` reaches main() but main()'s `write(1, "[mp-main-entered]", 18)`
   produces NUL bytes instead of the marker string**, then
   execution hits "Illegal/Unhandled opcode FFFF" → INT 6 →
   handler loops forever.
5. **Root cause: LE FIXUP records aren't being applied to absolute
   address references in MP.EXE under PMODE/W.** The push imm32
   for the marker string in MP.EXE's `_main` contains the
   data-section *offset* (0x0000c7a3), not the absolute VA
   (0x7c7a3 = data section base 0x70000 + 0xc7a3). At runtime,
   `_write` reads from address 0xc7a3 (zero-filled / unmapped),
   producing NUL bytes. Subsequent writes to other globals via
   `mov [imm32], eax` corrupt low memory, and eventually the
   CPU jumps to garbage code.
6. **ECHOTEST.EXE works through the same exe.py + wlink + PMODE/W
   path**, so the relocation problem is specific to large
   binaries or to some asm pattern uc386 emits for MicroPython
   that NASM/wlink mishandles.

### What we ruled out (further refinement)

A second wave of progressive runtime diagnostics (commits
04cdc50..f2faf76) added byte-level dumps and direct-call experiments
from the bridge stub. They proved:

- **LE FIXUP records ARE applied correctly.** The runtime
  imm32 in `_main`'s `push <str>` instruction is 0x000227a3
  (= obj2's runtime base 0x16000 + offset 0xc7a3), confirmed
  by reading memory at `_main + 7`.
- **The data section IS correctly loaded.** Bytes at runtime VA
  0x227a3 are "[mp-main-entered]\n" (`[str-bytes]=2d706d5b` =
  little-endian "[mp-").
- **`_main`'s instruction stream is intact at runtime.**
  `[main+0]=000004c8` = `enter 4, 0`; `[main+4]=a368126a` =
  push 18 + push opcode; `[main+11]=bde8016a` = push 1 +
  call rel32; `[main+14]=00002ebd` = the rel32 imm.
- **The call rel32 lands at the correct `_write`.** Computed
  target = `_main + 18 + 0x2ebd = 0x1c6573`; bytes at that
  address are `55 89 e5 8b 5d 08 8b 55 0c 8b 4d 10 b4 40 cd 21`
  — the literal _write prologue + INT 21h.
- **Direct INT 21h AH=0x40 from the bridge with the SAME args
  works.** `[direct-write]=[mp-main-entered]` proves DOS handles
  the redirect correctly for fd 1, EDX=0x227a3, ECX=18, BX=1.
- **A bridge-side clone of `_write` (`_bridge_write_stack`)
  with cdecl args from the bridge works.** `[stack-write]=`
  proves stack-based arg passing isn't the issue.
- **A bridge-side `_main` clone (`_diag_main_writelike`) doing
  the EXACT same enter/push/push/push/call sequence works.**
  `[mainlike]=` proves the instruction sequence itself isn't
  the issue.
- **Calling user.obj's `_write` DIRECTLY from the bridge (via
  indirect call to its runtime address) works.** `[user-write]=`
  proves user.obj's `_write` is reachable AND functional with
  the right register state. The runtime bytes are correct.

### Root cause CONFIRMED — the bug is DOSBox-X-specific

CI run `25479809993` ran the SAME MP.EXE binary under classic
DOSBox 0.74-3 (installed via `apt install dosbox`) instead of
DOSBox-X. Output (MPDOS.LOG):

    [mp-main-entered]
    [mp-before-mp-init]
    [mp-after-mp-init]
    [mp-before-repl]
    MicroPython uc386-triage on 2026-05-01; uc386-dos with i386
    Type "help()" for more information.
    >>> import lwip, uc386_net
    >>> lwip.reset()

The full MicroPython REPL boots cleanly — `mp_init`, the
`MicroPython uc386-triage` banner, REPL prompt, `import lwip`
all work. Execution stops at `lwip.reset()` only because classic
DOSBox doesn't have NE2000+SLIRP (no real packet driver, so
the network setup fails — but that's an unrelated, expected
limitation).

**Conclusion**: The `_main` → `_write` hang is a DOSBox-X bug,
not a PMODE/W or wlink or codegen issue. Specifically:
DOSBox-X's INT 21h emulation has caller-EIP-dependent behavior
that fails when the calling code lives at a high VA inside a
large (>256 KB) code object. The same `_write` body, called from
the bridge stub at ~0x171xxx, works; called from `_main` at
~0x1c36b3, it produces only NUL bytes.

The rig keeps DOSBox-X because we need NE2000+SLIRP to validate
the packet-driver path. But MP.EXE runtime testing should use
classic DOSBox (the rig's CI workflow now does both — see the
"Run MP.EXE under classic DOSBox 0.74-3" step in mp-rig.yml).
For network-stack validation under PMODE/W, real DOS hardware
or QEMU+SeaBIOS would be the right target, not DOSBox-X.

### QEMU + FreeDOS — the real-target proxy

The actual production target for this binary is FreeDOS (typical
deployment: VMware on Windows). Classic DOSBox 0.74-3 proves
"the binary works under a non-DOSBox-X DOS emulator", but DOSBox
isn't FreeDOS — it intercepts INT 21h in its own DOS layer.
QEMU+FreeDOS is the next step closer to real hardware: full PC
emulation booting an actual FreeDOS kernel, with PMODE/W and
INT 21h going through the same code path the production VM uses.

The CI workflow includes a "Run MP.EXE under QEMU+FreeDOS" step
that:

1. Boots `qemu-system-i386` headless (`-display none -serial stdio`)
   from a FreeDOS minimal floppy image
   (`codercowboy/freedosbootdisks/bootdisks/freedos.boot.disk.1.4MB.img`)
   with MP.EXE, ECHOTEST.EXE, and an AUTOEXEC.BAT injected via
   mtools.
2. Autoexec runs `CTTY COM1` so DOS console I/O lands on the
   serial port (which QEMU pipes to host stdout), then runs
   ECHOTEST as a baseline and MP.EXE bare (no I/O redirect, since
   the REPL reads via INT 21h AH=0x01 which doesn't honor `<`
   redirects from a CTTY-redirected shell).
3. Host-side, polls the captured log for "MicroPython" and stops
   QEMU as soon as the banner appears — that's proof the full
   `_main → _write → INT 21h AH=0x40` path works under FreeDOS.

Local Mac repro (verified 2026-05-07 on Darwin 25.4.0,
qemu-system-i386 from Homebrew):

    [bridge-entered] ... [bridge-pre-call-main]
    [mp-main-entered]
    [mp-before-mp-init]
    [mp-after-mp-init]
    [mp-before-repl]
    MicroPython uc386-triage on 2026-05-01; uc386-dos with i386
    Type "help()" for more information.
    >>>

The DOSBox-X-specific INT 21h bug does NOT manifest under FreeDOS
on QEMU. So the binary's only blocker for the production target
is whatever stops it inside DOSBox-X — which, per the bisect
above, is purely a DOSBox-X emulation defect.

### Filing this upstream

The DOSBox-X bug should be reported with a minimal repro:

- A PMODE/W binary with two `INT 21h AH=0x40 BX=1` call sites,
  one at low VA in obj 1, one at high VA in obj 1 (e.g., padded
  with ~256 KB of code between them). Same EDX/ECX args.
- Under DOSBox-X: the high-VA call writes NUL bytes; under
  classic DOSBox (or real DOS): both calls produce the
  expected output.

The mp-rig diagnostic infrastructure (commits `04cdc50..7c4545c`,
the `_bridge_emit` / `_bridge_emit_hex32` / `_bridge_write_stack`
machinery in `addons/harness/exe.py`) is ready-made for building
that minimal repro.

### Earlier hypothesis log (rejected/superseded)

1. ~~PMODE/W INT 21h passthrough for AH=0x40 doesn't reach the
   redirected RIG.LOG.~~ Rejected: ECHOTEST.EXE prints `hello dos rig`
   via the same path.
2. ~~`_pmodew_start` bridge doesn't initialize the 1.4 MB BSS
   correctly.~~ Rejected: PMODE/W's loader zero-fills BSS at load
   time per the LE bss_size header. The codegen rep stosb is
   redundant — and runs into a separate failure on multi-100KB
   ranges (suspect (2) above), which we now bypass.
3. ~~Some libc symbol the multi-TU MicroPython build references
   isn't provided by exe.py's bridge.~~ Rejected: linker would
   have erred on an unresolved external; the link is clean.

Until that's diagnosed, dos_emu's emulator (the existing
`test_micropython_smoke.py` suite — 94/94 passing) remains the
authoritative validation that the binary works. The DOSBox-X
rig is shipped as the on-ramp to real-hardware testing.

## Source attribution

`NE2000.COM` is the Crynwr 11.4.3 NE2000 packet driver, MIT-style
distribution terms (see `COPYING.DOC` shipped alongside the source
in the original Crynwr package). Fetched from the Internet Archive
mirror at `lan-packet-drivers-for-ms-dos/drvlan/ne2000.zip`.
