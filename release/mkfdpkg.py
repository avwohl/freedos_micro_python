#!/usr/bin/env python3
"""Build a FreeDOS package (and a drop-in FDNPKG repository) for MP.EXE.

The output conforms to the FreeDOS 1.1+ package layout documented at
https://help.freedos.org/docs/info/package.html and matches what the
official repository actually ships — the structure here was derived by
dissecting `bwbasic.zip` and `upx.zip` from

    https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/latest/devel/

Package layout produced:

    APPINFO/MPYTHON.LSM         package metadata (LSM v3, Begin3 ... End)
    DEVEL/MPYTHON/MP.EXE        the interpreter
    DEVEL/MPYTHON/README.TXT    DOS-side quick start
    DEVEL/MPYTHON/LICENSE.TXT   MIT + third-party notices
    DEVEL/MPYTHON/*.PY          bundled example programs
    LINKS/MP.BAT                puts MP on the %PATH%
    LINKS/MPYTHON.BAT           alias for discoverability
    SOURCE/MPYTHON/SOURCES.ZIP  port sources + reproduce instructions

Repository layout produced (a directory you can serve over plain HTTP
and point FDNPKG at with a single `REPO` line):

    repo/devel/mpython.zip
    repo/devel/index.lst        FD-REPOv1, tab separated, CRLF
    repo/devel/index.gz         gzip of index.lst
    repo/listing.csv            same metadata in the repo-wide CSV form

Run:

    python3 release/mkfdpkg.py --exe path/to/MP.EXE

Everything lands under release/fdpkg/ (gitignored).
"""

from __future__ import annotations

import argparse
import csv
import gzip
import io
import re
import subprocess
import textwrap
import time
import zipfile
import zlib
from datetime import date
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Package identity. The name must be <= 8 chars from [a-z0-9_] — it is
# both the zip filename and the LSM basename.
PKG = "mpython"
GROUP = "devel"           # interpreters live here: bwbasic, lua, euphoria, regina
CATEGORY_DIR = "DEVEL"    # the in-zip category directory for this group

# Upstream pins, mirrored from src/freedos_micro_python/scripts/fetch.sh.
# These are what SOURCES.ZIP tells a rebuilder to fetch.
PINS = {
    "micropython": ("https://github.com/micropython/micropython",
                    "9f396bba8d675ffb53f7fb047def21c7a581948e"),
    "axtls":       ("https://github.com/micropython/axtls",
                    "531cab9c278c947d268bd4c94ecab9153a961b43"),
    "lwip":        ("https://github.com/lwip-tcpip/lwip",
                    "STABLE-2_2_1_RELEASE"),
}


# --------------------------------------------------------------------------
# 8.3 validation
# --------------------------------------------------------------------------

_83_RE = re.compile(r"^[A-Za-z0-9_~!@#$%^&()\-{}']{1,8}(\.[A-Za-z0-9_~!\-]{1,3})?$")


def check_83(path: str) -> None:
    """Every component of an in-zip path must fit 8.3.

    FreeDOS packages are extracted on FAT without long-filename support,
    so a name that does not fit gets silently mangled. The one sanctioned
    exception is the contents of SOURCES.ZIP, which is never extracted by
    the package manager — it is copied through as a single 8.3 file.
    """
    for part in path.split("/"):
        if not part:
            continue
        if not _83_RE.match(part):
            raise SystemExit(f"mkfdpkg: not an 8.3 name: {part!r} (in {path!r})")


# --------------------------------------------------------------------------
# text helpers
# --------------------------------------------------------------------------

# Typographic characters cp437 has no room for. Encoding with
# errors="replace" would turn each into "?" — harmless in a comment,
# but wget.py raises OSError("ssl module not available — build with
# ...") and shipping that message with a "?" in it is just sloppy.
_TRANSLIT = {
    0x2014: "--",  0x2013: "-",   0x2192: "->",  0x2190: "<-",
    0x2018: "'",   0x2019: "'",   0x201C: '"',   0x201D: '"',
    0x2026: "...", 0x00A0: " ",   0x00B7: "*",   0x2022: "*",
}


def to_dos_text(text: str) -> str:
    return text.translate(_TRANSLIT)


def crlf(text: str) -> bytes:
    """DOS text files use CRLF. Normalise whatever we were handed.

    Refuses rather than silently substituting: encoding with
    errors="replace" would turn anything cp437 lacks into "?", and
    since the text legitimately contains question marks (URLs, prose)
    a "?" in the output is not something we could detect afterwards.
    """
    text = to_dos_text(text).replace("\r\n", "\n").replace("\n", "\r\n")
    bad = sorted({ch for ch in set(text) if not _encodable(ch)})
    if bad:
        detail = ", ".join(f"{ch!r} (U+{ord(ch):04X})" for ch in bad)
        raise SystemExit(
            f"mkfdpkg: cp437 cannot represent {detail}; add them to "
            "_TRANSLIT rather than shipping '?' to users")
    return text.encode("cp437")


def _encodable(ch: str) -> bool:
    try:
        ch.encode("cp437")
        return True
    except UnicodeEncodeError:
        return False


def lf(text: str) -> bytes:
    """LINKS/*.BAT marker files are LF-only in the official packages —
    FDNPKG reads the target path out of them, so match byte-for-byte."""
    return text.replace("\r\n", "\n").encode("ascii")


def wrap_lsm(field: str, value: str, width: int = 78) -> str:
    """Format one LSM field, wrapping continuations to column 17 the way
    the official LSMs do."""
    pad = 16
    label = f"{field}:"
    label = label.ljust(pad) if len(label) < pad else label + " "
    body = " ".join(value.split())
    chunks = textwrap.wrap(body, width=max(width - pad, 20)) or [""]
    return "\n".join([label + chunks[0]] + [" " * pad + c for c in chunks[1:]])


# --------------------------------------------------------------------------
# content builders
# --------------------------------------------------------------------------

def build_lsm(version: str, entered: str, modified: str, description: str,
              summary: str) -> str:
    def fld(name, value):
        return wrap_lsm(name, value)

    parts = [
        "Begin3",
        fld("Title", "MicroPython"),
        fld("Version", version),
        fld("Entered-Date", entered),
        fld("Description", description),
        fld("Summary", summary),
        fld("Keywords", "python, micropython, interpreter, repl, scripting, "
                        "language, tcp/ip, tls, ssh"),
        fld("Author", "Damien P. George and MicroPython contributors"),
        fld("Maintained-By", "freedos_micro_python contributors"),
        fld("Primary-Site", "https://github.com/avwohl/freedos_micro_python"),
        fld("Original-Site", "https://micropython.org/"),
        fld("Wiki-site", "https://avwohl.github.io/freedos_micro_python/"),
        fld("Platforms", "DOS (386 or better; built with the uc386 C23 "
                         "compiler, bound with the DOS/32A extender)"),
        fld("Copying-Policy", "Multiple open source licenses. See LICENSE "
                              "file."),
        fld("Long-file-names", "false"),
        fld("Modified-Date", modified),
        "End",
    ]
    return "\n".join(parts) + "\n"


README_TXT = """\
MicroPython for FreeDOS
=======================

MicroPython is a lean, complete implementation of Python 3 that runs
in a fraction of the memory CPython needs. This package is a native
FreeDOS / i386 build: a single MP.EXE with no runtime dependencies
beyond DOS itself.

  MicroPython {version}
  Built with the uc386 C23 compiler; bound with the DOS/32A extender.


Quick start
-----------

Start the interactive REPL:

    MP

Run a script and exit (extra words land in sys.argv):

    MP SCRIPT.PY [args ...]

Exit status is 0, or 1 on an uncaught exception. Press Ctrl-D (or call
sys.exit()) to leave the REPL.

You can also paste a whole program into the REPL: press Ctrl-E, paste
the text, then press Ctrl-D to run it. That needs no file at all.


What works
----------

Arithmetic (including arbitrary-precision integers and floats), control
flow, functions, closures, classes and inheritance, list/dict/set
comprehensions, generators, exception handling with tracebacks, string
formatting including f-strings, and roughly 25 builtins.

Modules: sys, os, os.path, time, math, re, json, random, struct,
binascii, hashlib, heapq, collections, io, errno, shutil, socket,
select, ssl, deflate, and more. Type help('modules') at the REPL for
the list this build actually carries.

File I/O works against real DOS files: open/read/write/append,
os.listdir, os.stat, os.remove, os.rename, os.mkdir, and shutil.


Networking
----------

Networking needs a packet driver loaded before MP.EXE starts — any
Crynwr-compatible driver will do (NE2000.COM, 3C509.COM, and so on).
Configure the interface at the DOS prompt, then use the socket module,
or the bundled programs:

    MP WGET.PY -O OUT.TXT https://example.com/file
    MP SCP.PY  user@host:/etc/motd MOTD.TXT
    MP SFTP.PY get user@host:/etc/hostname HOST.TXT

TLS is provided by axtls. SSH is provided by libssh2 with an axtls +
TweetNaCl crypto backend.


Where things are
----------------

    {catdir}\\{pkgu}\\MP.EXE       the interpreter
    {catdir}\\{pkgu}\\*.PY         the bundled programs
    {catdir}\\{pkgu}\\LICENSE.TXT  license and attribution
    SOURCE\\{pkgu}\\SOURCES.ZIP    complete port sources

Installing the package also puts MP on your %PATH% via a link in the
FreeDOS links directory, so MP works from any directory.


Documentation and source
------------------------

    Manual:  https://avwohl.github.io/freedos_micro_python/
    Source:  https://github.com/avwohl/freedos_micro_python
    Bugs:    https://github.com/avwohl/freedos_micro_python/issues

MicroPython itself: https://micropython.org/
"""


LICENSE_HEADER = """\
MicroPython for FreeDOS -- licenses and attribution
==================================================

This package combines several open source projects. The integration
glue is MIT; the components carry their own licenses, all of which are
open source and all of which permit redistribution in this form.

Full third-party attribution, including per-component copyright lines
and how each component is used, ships in SOURCES.ZIP as
docs/THIRD_PARTY.md.

Summary
-------

  MicroPython .......... MIT           (c) 2013- Damien P. George et al.
  lwIP ................. BSD 3-Clause  (c) 2001- SICS / Adam Dunkels
  axtls (MP fork) ...... BSD 3-Clause  (c) 2007-2016 Cameron Rich et al.
  libssh2 .............. BSD 3-Clause  (c) 2004- Daniel Stenberg et al.
  TweetNaCl ............ Public domain  D. J. Bernstein et al.
  crypto-algorithms .... Public domain  Brad Conte
  DOS/32A extender ..... BSD 3-Clause  (c) 1996-2006 Narech K.
  Integration glue ..... MIT           (c) 2026 freedos_micro_python
                                           contributors

The integration glue license follows in full.


"""


BUILD_TXT = """\
Building MicroPython for FreeDOS from source
============================================

This archive holds the complete source of the FreeDOS/i386 MicroPython
port -- everything that is specific to this package. The port is built
against pinned upstream trees that are fetched at build time rather than
vendored here, because they are large and unmodified apart from the
patches the fetch script applies (each patch is in the script, readable
and reproducible).

Pinned upstreams
----------------

{pins}

The fetch script applies its patches to these trees; read
src/freedos_micro_python/scripts/fetch.sh to see exactly what changes
and why.

Toolchain
---------

The port is compiled by uc386, a C23 compiler targeting i386 / MS-DOS:

    https://github.com/avwohl/uc386   (PyPI: uc386)

This build used uc386 {uc386_version}.

Build
-----

On a Unix-y host (macOS or Linux) with Python 3.10+:

    pip install uc386 freedos_micro_python

    mkdir mp-build && cd mp-build
    freedos-micropython fetch     # pulls the pinned upstreams above
    freedos-micropython build     # triage pass; generates qstrdefs
    freedos-micropython port      # multi-TU build -> build/micropython.bin

That takes roughly 25-30 minutes and produces build/micropython.bin
plus build/micropython.asm.

Turn the assembly into the DOS executable shipped in this package:

    python -m addons.harness.exe build/micropython.asm -o MP.EXE

The harness defaults to the DOS/32A extender, which is what this
package ships. PMODE/W remains selectable with --extender=pmodew, but
note that PMODE/W's real-mode call path wedges disk I/O under QEMU +
FreeDOS -- see docs/WIP.md for the full diagnosis.

Package
-------

Rebuild this FreeDOS package with:

    python3 release/mkfdpkg.py --exe MP.EXE
"""


# --------------------------------------------------------------------------
# sources archive
# --------------------------------------------------------------------------

def build_sources_zip(uc386_version: str) -> bytes:
    """The complete port source, as a single nested zip.

    Nesting is the established convention (bwbasic and upx both ship
    SOURCE/<pkg>/SOURCES.ZIP) and it neatly sidesteps the 8.3 rule for
    source trees, which the LSM spec explicitly exempts.
    """
    buf = io.BytesIO()
    tracked = subprocess.run(
        ["git", "ls-files"], cwd=REPO_ROOT,
        capture_output=True, text=True, check=True,
    ).stdout.split()

    skip_prefixes = ("rigs/",)  # test rigs: large, host-side, not port source
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        pins = "\n".join(
            f"  {name:<14} {url}\n  {'':<14} at {ref}"
            for name, (url, ref) in PINS.items()
        )
        z.writestr("BUILD.txt", BUILD_TXT.format(
            pins=pins, uc386_version=uc386_version))
        for rel in tracked:
            if rel.startswith(skip_prefixes):
                continue
            src = REPO_ROOT / rel
            if not src.is_file():
                continue
            z.write(src, rel)
    return buf.getvalue()


# --------------------------------------------------------------------------
# the package itself
# --------------------------------------------------------------------------

def build_package(exe: Path, version: str, entered: str, modified: str,
                  description: str, summary: str, uc386_version: str) -> bytes:
    pkgu = PKG.upper()
    prog_dir = f"{CATEGORY_DIR}/{pkgu}"

    entries: list[tuple[str, bytes]] = []

    entries.append((f"APPINFO/{pkgu}.LSM",
                    crlf(build_lsm(version, entered, modified,
                                   description, summary))))

    entries.append((f"{prog_dir}/MP.EXE", exe.read_bytes()))
    entries.append((f"{prog_dir}/README.TXT",
                    crlf(README_TXT.format(version=version, catdir=CATEGORY_DIR,
                                           pkgu=pkgu))))
    entries.append((f"{prog_dir}/LICENSE.TXT",
                    crlf(LICENSE_HEADER
                         + (REPO_ROOT / "LICENSE").read_text())))

    # Bundled programs. These double as the project's networking
    # regression tests, so they are known-good against real servers.
    for py in sorted((REPO_ROOT / "examples").glob("*.py")):
        entries.append((f"{prog_dir}/{py.name.upper()}",
                        crlf(py.read_text())))

    # LINKS: one marker batch file per executable we want on the %PATH%.
    # Content is the in-zip path to the target; FDNPKG rewrites it at
    # install time. LF-only, matching the official packages.
    link_target = f"{CATEGORY_DIR.lower()}\\{PKG}\\mp.exe\n"
    entries.append(("LINKS/MP.BAT", lf(link_target)))
    entries.append((f"LINKS/{pkgu}.BAT", lf(link_target)))

    entries.append((f"SOURCE/{pkgu}/SOURCES.ZIP",
                    build_sources_zip(uc386_version)))

    for name, _ in entries:
        check_83(name)

    buf = io.BytesIO()
    # Deflate only: FDNPKG also supports LZMA, but LZMA decompression
    # needs ~24 MiB, more than most DOS machines have.
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in entries:
            info = zipfile.ZipInfo(name, date_time=time.localtime()[:6])
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, data)
    return buf.getvalue()


# --------------------------------------------------------------------------
# repository
# --------------------------------------------------------------------------

def build_index_lst(rows: list[dict], group: str, build_time: int) -> bytes:
    """FDNPKG's per-group repository index.

    Format, verified against the live repository:

        FD-REPOv1<TAB>Build time: <epoch><TAB><group><TAB><count>CRLF
        <pkg><TAB><version><TAB><description><TAB><CRC32>CRLF
    """
    out = [f"FD-REPOv1\tBuild time: {build_time}\t{group}\t{len(rows)}"]
    for r in rows:
        out.append(f"{r['package']}\t{r['version']}\t{r['description']}\t{r['crc32']}")
    return ("\r\n".join(out) + "\r\n").encode("cp437", "replace")


FDNPKG_CFG_SAMPLE = """\
# Add this line to your FDNPKG.CFG to install MicroPython over the
# network. FDNPKG reads %DOSDIR%\\BIN\\FDNPKG.CFG by default; you can
# point it somewhere else with
#
#     SET FDNPKG.CFG=C:\\MYDIR\\FDNPKG.CFG
#
# after which:
#
#     FDNPKG install mpython
#
# A packet driver and a working WATTCP.CFG are required for network
# repositories -- the same setup MP.EXE's own socket support needs.

REPO {url}/{group}
"""


def build_repo(outdir: Path, pkg_bytes: bytes, row: dict, group: str,
               base_url: str) -> None:
    repo = outdir / "repo"
    gdir = repo / group
    gdir.mkdir(parents=True, exist_ok=True)

    (gdir / f"{PKG}.zip").write_bytes(pkg_bytes)

    build_time = int(time.time())
    index = build_index_lst([row], group, build_time)
    (gdir / "index.lst").write_bytes(index)
    # mtime=0 keeps the gzip byte-identical across runs with the same input.
    (gdir / "index.gz").write_bytes(gzip.compress(index, 9, mtime=0))

    with (repo / "listing.csv").open("w", newline="", encoding="utf-8") as fh:
        cols = ["package", "title", "group", "modified-date", "entered-date",
                "version", "author", "copying-policy", "crc32", "description",
                "summary", "keywords"]
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
        w.writerow({c: row.get(c, "") for c in cols})

    (repo / "FDNPKG.CFG.SAMPLE").write_text(
        FDNPKG_CFG_SAMPLE.format(url=base_url.rstrip("/"), group=group))


# --------------------------------------------------------------------------

def uc386_version() -> str:
    try:
        import uc386
        return uc386.__version__
    except Exception:
        return "unknown"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", type=Path, required=True,
                    help="the built MP.EXE to package")
    ap.add_argument("--version", default=None,
                    help="package version (default: the project version)")
    ap.add_argument("--outdir", type=Path, default=REPO_ROOT / "release" / "fdpkg")
    ap.add_argument("--base-url",
                    default="https://avwohl.github.io/freedos_micro_python/repo",
                    help="URL the repo will be served from (for FDNPKG.CFG)")
    ap.add_argument("--entered-date", default=None,
                    help="LSM Entered-Date (default: today)")
    ns = ap.parse_args()

    if not ns.exe.is_file():
        raise SystemExit(f"mkfdpkg: no such file: {ns.exe}")

    version = ns.version
    if version is None:
        import freedos_micro_python
        version = getattr(freedos_micro_python, "__version__", None)
        if version is None:
            from importlib.metadata import version as _v
            version = _v("freedos_micro_python")

    today = ns.entered_date or date.today().isoformat()
    description = "Python 3 interpreter and REPL"
    summary = (
        "MicroPython is a lean and complete implementation of Python 3 that "
        "runs in a fraction of the memory CPython needs. This is a native "
        "FreeDOS / i386 build: a single MP.EXE with no runtime dependencies "
        "beyond DOS. It provides an interactive REPL and runs scripts from "
        "the command line, with arbitrary-precision integers, floats, "
        "classes, closures, generators, comprehensions and exception "
        "handling, plus os, os.path, time, math, re, json, random, struct, "
        "binascii, hashlib, shutil, socket, select and ssl. File I/O works "
        "against real DOS files. Networking runs over any Crynwr-compatible "
        "packet driver and includes TLS (axtls) and SSH (libssh2); wget, scp "
        "and sftp programs are bundled. Built with the uc386 C23 compiler "
        "and bound with the DOS/32A extender."
    )

    ucv = uc386_version()
    pkg_bytes = build_package(ns.exe, version, today, f"{today}.0",
                              description, summary, ucv)

    crc = "%08X" % (zlib.crc32(pkg_bytes) & 0xFFFFFFFF)

    ns.outdir.mkdir(parents=True, exist_ok=True)
    pkg_path = ns.outdir / f"{PKG}.zip"
    pkg_path.write_bytes(pkg_bytes)

    row = {
        "package": PKG,
        "title": "MicroPython",
        "group": GROUP,
        "modified-date": f"{today}.0",
        "entered-date": today,
        "version": version,
        "author": "Damien P. George and MicroPython contributors",
        "copying-policy": "Multiple open source licenses. See LICENSE file.",
        "crc32": crc,
        "description": description,
        "summary": summary,
        "keywords": "python, micropython, interpreter, repl, scripting, language",
    }
    build_repo(ns.outdir, pkg_bytes, row, GROUP, ns.base_url)

    print(f"mkfdpkg: {pkg_path}  ({len(pkg_bytes):,} bytes, CRC32 {crc})")
    print(f"mkfdpkg: repo at {ns.outdir / 'repo'}")
    with zipfile.ZipFile(io.BytesIO(pkg_bytes)) as z:
        for i in z.infolist():
            print(f"    {i.file_size:>9,}  {i.filename}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
