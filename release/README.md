# Release area

This directory is where build artifacts and source archives intended
for distribution alongside a release live. The committed contents
are:

  - `README.md` — this file.
  - `fetch-freedos.sh` — reproducible fetch script that pulls the
    FreeDOS components we reference and bundles them into
    `release/freedos-sources-<date>.tar.gz`.

The actual tarballs are **not** checked into git (gitignored via
`release/.gitignore`). Cut a release by running the fetch script;
the resulting tarballs go alongside `MP.EXE` and the bound source
release artifacts.

## Why ship FreeDOS sources

This project targets FreeDOS but does not modify it. Strictly
speaking, MIT-licensed glue calling into FreeDOS at the system-call
boundary does not trigger any source-redistribution obligation.

However, FreeDOS is the operating system that makes any of this
useful, and we leaned heavily on the FreeDOS source tree while
debugging PMODE/W's INT 21h reflection, the DOS packet-driver
interface, the FAT write path, and a handful of NLS / RTC quirks.

In a past project (a DOS emulator), the FreeDOS community asked
that distributors of anything that *uses* anything FreeDOS pass the
sources along, regardless of the strict letter of the license. That
seems like good practice. We do it here without regard to whether
our limited use requires it.

## What `fetch-freedos.sh` pulls

  - **FreeDOS kernel** — github.com/FDOS/kernel, the most recent
    tagged release at run-time.
  - **FreeCOM** (`COMMAND.COM`) — github.com/FDOS/freecom,
    most recent tag.
  - **PMODE/W** — Tran's DOS extender; pinned to the
    1.33 release. The source is part of the PMODE/W zip.
  - **The 1.4 MB boot disk image we test against** —
    `codercowboy/freedosbootdisks/freedos.boot.disk.1.4MB.img`,
    which is the same artifact the rigs download at run time.

The script verifies SHA-256 sums against pinned values in the
script body. Bumping a pin is a one-line edit + a re-run.

## Tarball layout

After `./fetch-freedos.sh`, you get
`release/freedos-sources-YYYY-MM-DD.tar.gz` with this layout:

```
freedos-sources/
  README.md            — explains what's inside and why
  LICENSE-summary.md   — per-component license summary
  kernel/              — FDOS kernel source tree
  freecom/             — FreeCOM source tree
  pmodew/              — PMODE/W zip (extract to inspect)
  bootdisk/            — the 1.4 MB image we boot under qemu
```

Each component's own LICENSE / COPYING file ships unmodified in
its subdirectory.

## Running

```sh
cd release/
./fetch-freedos.sh
# produces release/freedos-sources-YYYY-MM-DD.tar.gz
```

Wall-clock: a couple of minutes plus download time. Total size
is roughly 15–25 MB depending on which kernel/freecom tags are
current.
