# Draft submission to freedos-devel

Not sent. This is a draft for a human to review, complete and send to
<freedos-devel@lists.sourceforge.net> (subscribe at
<https://sourceforge.net/p/freedos/mailman/freedos-devel/>).

**Before sending, you must:**

1. Fill in the provenance paragraph marked `[TODO]`. See
   [`docs/FREEDOS_PACKAGING.md`](../docs/FREEDOS_PACKAGING.md) —
   FreeDOS has a No-AI policy scoped to core packages. An optional
   `devel` package falls outside that scope, but the community cares
   about provenance and this should be stated accurately and in your
   own words rather than left to be discovered.
2. Host `mpython.zip` at a stable URL and paste it in.
3. Confirm the test-install under QEMU + FreeDOS actually passed, and
   say which FreeDOS version you tested on.

---

**Subject:** New package for the repository: mpython (MicroPython for
FreeDOS)

Hello,

I would like to submit a new package for the `devel` group:
**MicroPython**, a native FreeDOS / i386 build of Python 3.

There is currently no Python of any kind in the repository — I checked
all 365 packages in `listing.csv` — so this fills a gap rather than
duplicating anything.

**What it is**

MicroPython is a lean, complete implementation of Python 3 that runs in
a fraction of the memory CPython needs. This build is a single
`MP.EXE`, about 600 KB, with no runtime dependencies beyond DOS. It
gives you an interactive REPL and runs scripts from the command line:

    MP                      starts the REPL
    MP SCRIPT.PY [args]     runs a script; extra words land in sys.argv

Working: arbitrary-precision integers, floats, classes and inheritance,
closures, generators, comprehensions, exceptions with tracebacks,
f-strings, and about 25 builtins. Modules include `sys`, `os`,
`os.path`, `time`, `math`, `re`, `json`, `random`, `struct`,
`binascii`, `hashlib`, `heapq`, `collections`, `io`, `errno`,
`shutil`, `socket`, `select`, `ssl` and `deflate`. File I/O works
against real DOS files.

Networking runs over any Crynwr-compatible packet driver, with TLS via
axtls and SSH via libssh2. `WGET.PY`, `SCP.PY` and `SFTP.PY` ship in
the package and work end-to-end against real servers.

**Build**

Compiled with uc386, a C23 compiler that targets i386 / MS-DOS, and
bound with the DOS/32A extender. It is not built with OpenWatcom, so I
have noted the toolchain in the LSM `Platforms` field as the spec
suggests for unusual compilers. The whole thing rebuilds from source
with two commands; `SOURCES.ZIP` in the package has the instructions
and the exact upstream commit pins.

I should mention one thing about DOS/32A specifically: I originally
built with PMODE/W and hit a hang on any DOS call that touches a
physical sector under QEMU + FreeDOS. From the QEMU monitor the CPU
sits in the BIOS with every IRQ masked, waiting on a completion flag
its own masked handler can never set. DOS/32A does not have the
problem. Writing it down in case it saves someone else the week it
cost me.

**Licensing**

Multiple open source licenses, all permissive, all redistributable:
MicroPython is MIT; lwIP, axtls, libssh2 and the DOS/32A stub are BSD
3-Clause; TweetNaCl and Brad Conte's crypto-algorithms are public
domain; the integration glue is MIT. Every component is catalogued
with its copyright line and its role in `docs/THIRD_PARTY.md` inside
`SOURCES.ZIP`. The LSM `Copying-Policy` reads "Multiple open source
licenses. See LICENSE file." per the spec's guidance for this case.

**Provenance**

[TODO — describe honestly how the port was developed, including any
use of AI tooling. The No-AI policy's Scope section covers core
packages and anything installed by a plain DOS installation; an
optional devel-group application is outside it. Say so plainly, say
what is verifiable — MIT licensing throughout, pinned upstreams,
reproducible build — and let the maintainers decide with full
information.]

**The package**

  <URL TO mpython.zip>

  Package:  mpython
  Group:    devel
  Version:  <VERSION>
  CRC32:    <CRC32>

It follows the 1.1+ layout: `APPINFO/MPYTHON.LSM`,
`DEVEL/MPYTHON/`, `LINKS/MP.BAT` and `LINKS/MPYTHON.BAT`, and
`SOURCE/MPYTHON/SOURCES.ZIP`. Nothing at the zip root, everything 8.3,
Deflate rather than LZMA. Tested installing with FDNPKG on
<FREEDOS VERSION> under <QEMU / real hardware>.

Happy to make changes if anything about the packaging is wrong — this
is my first submission and I would rather fix it than have you work
around it.

Thanks,
<NAME>

Source:  https://github.com/avwohl/freedos_micro_python
Manual:  https://avwohl.github.io/freedos_micro_python/
