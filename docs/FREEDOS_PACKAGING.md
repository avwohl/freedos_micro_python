# Shipping MicroPython as a FreeDOS package

This is the working note for getting `MP.EXE` into FreeDOS the way
FreeDOS users expect to get software: as a package their package
manager installs, not a zip they unpack by hand.

Everything below was verified against the live repository in August
2026 by dissecting real packages (`bwbasic.zip`, `upx.zip`,
`fdnpkg.zip`) rather than by reading documentation alone, because
several details in the published spec differ from what the repository
actually ships.

## The pieces, and what they are called

The user asked about "impulse". The tool is **FDIMPLES** — *FreeDOS
Installer - My Package List Editor Software*. It is worth being
precise about the four names, because they are different programs:

| Name | What it is |
| --- | --- |
| **FDI** | The FreeDOS installer that runs off the install media |
| **FDIMPLES** | Package selector add-on to FDI. Installs/removes packages from **local media** (CD, USB, a directory) |
| **FDNPKG** | Standalone **network** package manager. Installs from HTTP/gopher repositories, or from a local `.zip` |
| **FDRepo** | Jerome Shidel's **server-side** software that builds the official repository — generates the indexes, web pages and RSS feeds |

All of them consume the same package format, so one artifact serves
every route.

## Package format

A FreeDOS package is a plain **zip** file, Deflate-compressed. LZMA is
supported by FDNPKG but should not be used — decompression needs
~24 MiB, more than most DOS machines have.

Rules that actually matter:

- The package **name** is at most 8 characters from `[a-z0-9_]`, and
  the file is always `NAME.ZIP`. Ours is `mpython`.
- Every path in the zip must fit **8.3**. The one exception is a
  source tree, which is why the convention is to nest it as a single
  `SOURCES.ZIP` (both `bwbasic` and `upx` do this).
- **Nothing at the zip root.** FDRepo rejects packages with files in
  the root directory because they cause filename collisions at install
  time.
- Source code inclusion is expected.

Our layout:

```
APPINFO/MPYTHON.LSM         metadata (LSM v3: Begin3 … End)
DEVEL/MPYTHON/MP.EXE        the interpreter
DEVEL/MPYTHON/README.TXT    DOS-side quick start (CRLF)
DEVEL/MPYTHON/LICENSE.TXT   license + attribution summary
DEVEL/MPYTHON/WGET.PY       bundled programs, which double as
DEVEL/MPYTHON/SCP.PY        the project's networking regression
DEVEL/MPYTHON/SFTP.PY       tests
LINKS/MP.BAT                puts MP on the %PATH%
LINKS/MPYTHON.BAT           alias, same target
SOURCE/MPYTHON/SOURCES.ZIP  complete port source + BUILD.txt
```

`DEVEL` is the category directory for the `devel` group. The package
manager may relocate it at install time — `FDNPKG.CFG` maps
`dir devel c:\devel` by default — so nothing inside may assume a path.

### LINKS files

A file in `LINKS/` is a marker, not a working batch file. It contains
only the in-zip path of its target:

```
devel\mpython\mp.exe
```

The package manager rewrites it with real content at install time,
pointing at wherever the program actually landed. Note the official
packages write these **LF-only**, not CRLF; we match that byte-for-byte
because FDNPKG parses the path out of the file.

The extension changes on the way in. Verified by installing under
FreeDOS (see `rigs/fdpkg-rig/`): `LINKS/MP.BAT` in the zip becomes an
~80 byte **`MP.COM`** launcher in `%DOSDIR%\LINKS\`, not a batch file.
So ship `.BAT`, but expect `.COM` on disk.

### The LSM

`APPINFO/<PKG>.LSM` is a Linux Software Map v3 record: `Begin3`, then
`Field:` lines with continuations indented to column 17, then `End`.
Required: `Title`, `Version`, `Entered-Date`, `Description`, `Summary`,
`Author`, `Copying-Policy`.

Two notes from real packages that the spec page does not make obvious:

- The maintainer field is spelled **`Maintained-By:`** in practice, not
  `Maintainer:`.
- `Copying-Policy` wants the full unabridged license name — not `MIT`,
  not `GPLv2`. For a package combining several licenses the sanctioned
  wording is *"Multiple open source licenses. See LICENSE file."*,
  which is what we use, since MP.EXE links MicroPython (MIT), lwIP,
  axtls and libssh2 (BSD 3-Clause), TweetNaCl and crypto-algorithms
  (public domain), and the DOS/32A stub (BSD).

`Modified-Date: YYYY-MM-DD.N` is managed by the repository — FDRepo
corrects it if it is missing or wrong, so do not agonise over it.

### Repository format

A repository is a directory **per group**, each holding the `.zip`
files plus an index. FDNPKG is pointed at each group directory
separately, one `REPO` line apiece:

```
REPO http://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.4/devel
```

The index is `index.lst`, tab-separated with CRLF endings, alongside a
gzip of itself as `index.gz`:

```
FD-REPOv1<TAB>Build time: 1783603776<TAB>devel<TAB>54
bwbasic<TAB>3.30<TAB>The Bywater BASIC interpreter<TAB>C48ADF75
```

The trailing field is the **CRC-32 of the package zip file itself** —
verified: `bwbasic.zip` hashes to exactly `C48ADF75`. The repo-wide
`listing.csv` one level up carries the same data plus author, license,
summary and keywords.

`release/mkfdpkg.py` generates all of this.

## Verified by actually installing it

`rigs/fdpkg-rig/` boots a real FreeDOS kernel under QEMU and installs
the package with the real FreeDOS installer, then checks the resulting
disk image rather than the console output. Three things it established
that are not obvious from the spec:

- **`%TEMP%` must be set** or the installer prints `%TEMP% not set!`
  and exits having extracted nothing. Worth knowing because a user
  whose environment lacks it will see an install that appears to do
  nothing.
- **`LINKS/*.BAT` arrives as `*.COM`** — see above.
- **Sources are not extracted by default.** `installsources 0` is the
  FreeDOS default, so `SOURCE/MPYTHON/SOURCES.ZIP` costs the user disk
  space only if they ask for it (`FDNPKG install-wsrc`). Shipping the
  full source is therefore cheap for people who don't want it.

A clean run installs 9 files with 0 errors and the interpreter runs
from the path the installer chose.

## Groups

The official groups are `base`, `tools`, `apps`, `archiver`, `boot`,
`devel`, `disk`, `drivers`, `edit`, `emulator`, `games`, `gui`, `net`,
`sound`, `unix`, `util`.

MicroPython belongs in **`devel`**, which is where the other language
implementations live: `bwbasic`, `lua`, `euphoria`, `regina` (REXX),
`fpc` (Free Pascal), `fbc` (FreeBASIC), plus the DJGPP and IA-16
toolchains.

Worth stating plainly: **the FreeDOS repository has no Python at all.**
All 365 packages in `listing.csv`, and there is no Python, MicroPython
or any Python-adjacent interpreter. This fills a real gap rather than
duplicating something.

## Installing it today, before any FreeDOS release

Three routes, all working now, none requiring FreeDOS to accept
anything first.

**1. Local zip — simplest, no network.** Copy `mpython.zip` to the DOS
machine and:

```
FDNPKG install MPYTHON.ZIP
```

FDNPKG installs local zip files directly. `FDINST.EXE`, the small
non-network installer that ships alongside it, does the same on pre-386
machines.

**2. Our own network repository.** `release/mkfdpkg.py` emits a
ready-to-serve `repo/` tree. Publish it at any HTTP URL and users add
one line to their `FDNPKG.CFG`:

```
REPO http://example.org/repo/devel
```

then `FDNPKG install mpython`. This also gets them updates via
`FDNPKG checkupdates`. Requires a packet driver and a `WATTCP.CFG` —
the same setup MP.EXE's own networking needs, so our audience already
has it.

**3. Unzip by hand.** The layout is designed so that extracting the zip
over `C:\` puts everything in the right place.

## Getting into the official stream

There is no web form or pull-request queue for the package repository.
The route is:

1. **Post to `freedos-devel`**
   (<https://sourceforge.net/p/freedos/mailman/freedos-devel/>) —
   announce the package, link the zip, describe what it is. This is
   where package submissions are discussed and where Jerome Shidel,
   who runs FDRepo and the repository, picks them up.
2. Host the `.zip` somewhere stable and link it. The repository
   maintainer pulls the artifact; you do not push to ibiblio.
3. It lands first in the **Latest/Unstable** repository
   (<https://fd.lod.bz/repos>, mirrored to ibiblio under
   `repositories/latest/`), which is continuously updated. Inclusion
   there is what makes it installable via stock FDNPKG **without any
   config change by the user** — this is the real goal, and it does not
   wait for a FreeDOS release.
4. Inclusion in the *distribution* (the BonusCD / install media that
   FDIMPLES reads) is a separate, slower decision made when a release
   is assembled.

So the answer to "can users install it before the next release" is yes,
twice over: immediately via route 1 or 2 above, and — once accepted
into Latest/Unstable — via plain `FDNPKG install mpython` with no
special configuration.

## Provenance

This port is AI-written, and both this repository and uc386 say so at
the top of their README. The submission draft in
`release/freedos-devel-draft.md` leads with the same disclosure rather
than burying it.

FreeDOS has its own policy on AI-written code, and its scope is theirs
to determine — read it at <https://www.freedos.org/about/devel/> and let
the maintainers apply it. Paraphrasing it here would only invite
arguing with them about the meaning of their own rules, which is not our
business and not a good look on a first submission.

What we can usefully bring to that conversation is evidence, so the
submission answers the questions a maintainer would reasonably have:

- **Licensing** — every component is catalogued with its license and
  copyright in [`THIRD_PARTY.md`](THIRD_PARTY.md), and every upstream is
  pinned to an exact commit, so what we ship can be diffed against what
  upstream published.
- **Copyrightability** — if AI output is not eligible for copyright, the
  practical effect is that the integration glue is *more* permissive
  than its MIT notice claims, not less. The third-party components carry
  their own authors' copyrights and are untouched by this.
- **Correctness** — answered by behaviour, not byline. The build
  reproduces from source with two commands and is tested on real FreeDOS
  under QEMU, including `rigs/fdpkg-rig/`, which installs this very
  package with the real installer and runs the result.

If the answer is no, that is a legitimate answer and the right one to
accept. Being told no by people who had the facts beats being accepted
by people who didn't.

Note this cuts against the package in one concrete way: the LSM
`Author` field credits MicroPython's authors and `Maintained-By`
credits this project, which is accurate. Nothing in the metadata
overclaims.

## Status checklist

- [x] Package format understood and verified against live packages
- [x] `release/mkfdpkg.py` builds a conforming `mpython.zip`
- [x] Repository tree (`index.lst`, `index.gz`, `listing.csv`) generated
- [x] 8.3 validation enforced at build time; nothing at the zip root
- [x] Complete source shipped as `SOURCE/MPYTHON/SOURCES.ZIP`
- [x] Test-installed under QEMU + FreeDOS with the real FreeDOS
      installer — `rigs/fdpkg-rig/`, 6/6 checks including running the
      installed `MP.EXE`
- [ ] Host the repo at a stable URL
- [ ] Post to `freedos-devel` (draft in `release/freedos-devel-draft.md`
      — needs its provenance paragraph written before it goes out)
