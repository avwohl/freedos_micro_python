# Third-party software

`freedos_micro_python` is glue between several third-party projects.
This file catalogs every project whose source we redistribute,
fetch at build time, or rely on as the runtime target. Each entry
lists the upstream URL, the license, and where the bytes live.

The licenses below are the SHORT form. Authoritative full-text
licenses live with the upstream source tree of each project; the
fetch scripts pull them alongside the code, and any source copy we
redistribute (currently FreeDOS, in `release/`) ships its license
files intact.

---

## Target operating system

### FreeDOS

  - Upstream: https://www.freedos.org/ — sources at
    https://github.com/FDOS and https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/
  - License: **GPLv2** for the kernel and most utilities;
    individual components carry their own notices (FreeCOM is
    GPLv2; PMODE/W is "free for any use" per Tran's notice; some
    utilities use BSD or public-domain).
  - Copyright: © 1994–present The FreeDOS Project and individual
    contributors. Kernel © Pat Villani et al. FreeCOM © Steffen
    Kaiser, Aitor Santamaría Merino, et al. PMODE/W © Charles
    "Tran" Scheffold and Thomas Pytel.
  - How we use it: as the runtime target. `MP.EXE` is built for
    DOS-32 (PMODE/W bound) and runs against a FreeDOS kernel +
    COMMAND.COM. We do not modify FreeDOS sources.
  - Where it lives in this repo: the rigs pull the
    `codercowboy/freedosbootdisks` 1.4 MB boot image at run time
    (not committed). A source archive of the FreeDOS components
    we leaned on for debugging is in `release/` (see
    `release/README.md` for the fetch / build instructions —
    sources aren't checked into git but `release/fetch-freedos.sh`
    produces the tarball reproducibly).

## MicroPython port and runtime

### MicroPython

  - Upstream: https://github.com/micropython/micropython
  - License: **MIT**
  - Copyright: © 2013–present Damien P. George and contributors.
  - How we use it: this project IS a MicroPython port. We fetch
    pinned upstream as a tarball, generate the qstr table, and
    compile it through `uc386`. We add port-specific C in
    `src/freedos_micro_python/port/*.c` and shell glue in
    `scripts/`.
  - Where it lives: `upstream/` inside the build directory
    (`/private/tmp/fdmp-build/upstream/` by default). Not in git.

### lwIP

  - Upstream: https://savannah.nongnu.org/projects/lwip — mirrored
    at https://github.com/lwip-tcpip/lwip
  - License: **BSD 3-Clause**
  - Copyright: © 2001–present Swedish Institute of Computer
    Science and Adam Dunkels.
  - How we use it: the TCP/IP stack behind MicroPython's `socket`
    module. NE2000 packet-driver glue is ours
    (`port/pktdrv_uc386dos.c`); lwIP itself is unmodified except
    for one upstream-irrelevant patch in `fetch.sh`
    (`patch_modlwip_loopback_poll`).
  - Where it lives: `upstream/lib/lwip/`. Pinned to STABLE-2_2_1.

### axtls (MicroPython fork)

  - Upstream: https://github.com/micropython/axtls (a fork of
    Cameron Rich's axtls-2.1.x, maintained for MicroPython)
  - License: **BSD 3-Clause**
  - Copyright: © 2007–2016 Cameron Rich; © 2014–present
    MicroPython contributors for the fork.
  - How we use it: TLS, SHA-1/256/384/512, MD5, HMAC, AES-CBC,
    RSA, and bigint primitives. Our SSH crypto backend at
    `port/libssh2_axtls.c` calls into axtls. We added AES-CTR
    support on top of axtls's AES_encrypt.
  - Where it lives: `upstream/lib/axtls/`.

### libssh2

  - Upstream: https://www.libssh2.org/
  - License: **BSD 3-Clause** (and revised BSD)
  - Copyright: © 2004–present Daniel Stenberg et al.
  - How we use it: SSH-2 transport + KEX + auth + channel
    handling. We ship a custom crypto backend
    (`port/libssh2_axtls.{c,h}`) that maps libssh2's `crypto.h`
    onto axtls + TweetNaCl. libssh2 itself is unmodified except
    for the patches in `fetch.sh` (callback-macro joins, BSD
    typedef shims, ed25519 hoist outside `#if LIBSSH2_ECDSA`).
  - Where it lives: `upstream/lib/libssh2/`. Pinned to 1.11.1.

### TweetNaCl

  - Upstream: https://tweetnacl.cr.yp.to (mirror at
    https://github.com/ultramancool/tweetnacl-usable)
  - License: **Public domain**
  - Authors: Daniel J. Bernstein, Bernard van Gastel, Wesley
    Janssen, Tanja Lange, Peter Schwabe, Sjaak Smetsers.
  - How we use it: Curve25519 (X25519) for SSH KEX and Ed25519
    verify for SSH hostkey signatures.
  - Where it lives: `upstream/lib/tweetnacl/`. Patched in
    `fetch.sh` for uc386-dos stack-frame sizing (file-static
    hoist of large gf locals); compound-assign workarounds in
    that same patch are obsoleted by uc386 commit `10b4dfd` and
    pending removal — see `docs/WIP.md`.

### crypto-algorithms (Brad Conte)

  - Upstream: https://github.com/B-Con/crypto-algorithms
  - License: **Public domain**
  - Author: Brad Conte.
  - How we use it: MicroPython's `hashlib` uses Brad Conte's
    SHA-256 reference impl directly (see
    `upstream/extmod/modhashlib.c` includes
    `lib/crypto-algorithms/sha256.c`).
  - Where it lives: `upstream/lib/crypto-algorithms/`.

## Test fixtures

### paramiko

  - Upstream: https://github.com/paramiko/paramiko
  - License: **LGPL-2.1** (or later)
  - How we use it: paramiko runs as the SSH server fixture for
    `rigs/ssh-rig/run-ssh-rig.sh`. It is not bundled into the
    DOS binary; it runs on the host Python during the test.
  - Where it lives: installed via the project's dev env, not
    redistributed by us.

### codercowboy/freedosbootdisks

  - Upstream: https://github.com/codercowboy/freedosbootdisks
  - License: matches the bundled FreeDOS components (mostly GPLv2)
  - How we use it: the rigs fetch the 1.4 MB FreeDOS boot floppy
    image at run time and use it as the boot disk under QEMU.
    Not redistributed in this repo; downloaded by the rig
    scripts on first run.

## Compiler toolchain

### uc386

  - Upstream: https://github.com/avwohl/uc386 (PyPI: `uc386`)
  - License: see uc386's own LICENSE.
  - How we use it: uc386 is the C23 compiler that turns the
    MicroPython sources into a flat i386 binary. It also hosts
    the `dos_emu` test harness used by `tests/test_smoke.py`.
  - Where it lives: installed as a pip dependency; not bundled.

---

## Reporting a missing attribution

If you spot a third-party project we use without proper attribution
here, please open an issue at
https://github.com/avwohl/freedos_micro_python/issues — we treat
attribution gaps as bugs.
