# fdpkg-rig — install the FreeDOS package the way a user would

This rig proves the *packaging*, not the interpreter. It boots a real
FreeDOS kernel under QEMU and installs `mpython.zip` with the real
FreeDOS package installer, then checks that what landed on disk is what
the package promised.

Host-side zip inspection can only tell you the archive looks right.
This tells you the installer agrees.

## What it does

1. Boots the 1.44 MB FreeDOS floppy as `A:`, with a 32 MB partitioned
   FAT16 disk as `C:`.
2. Runs `FDINST install MPYTHON.ZIP` on the guest. `FDINST.EXE` is
   pulled from the official `fdnpkg` package — its docs say it "shares
   most of its source code with FDNPKG to ensure that both tools handle
   packages exactly the same way", so this exercises the same unpack
   path a user's `FDNPKG install mpython` would.
3. Runs the installed `MP.EXE` against a small script.
4. Checks the resulting **disk image** with mtools, rather than
   grepping the console — console text is easy to match by accident.

## Running

```sh
python3 release/mkfdpkg.py --exe /path/to/MP.EXE   # build the package
rigs/fdpkg-rig/run-fdinst-rig.sh                   # install it
```

Needs `qemu-system-i386` and `mtools`. Takes under a minute. The
captured COM1 log is `qemu-fdinst.log`; the installed disk is left
behind as `fdinst-hdd.img` so you can poke at it with `mdir`/`mtype`
using the generated `_mtoolsrc`.

## Things this rig found

- **FDINST refuses to run without `%TEMP%`.** It prints
  `%TEMP% not set!` and exits without extracting anything — but its
  exit status still looks unremarkable from a batch file, so a rig that
  only checked the console would have called it a pass.
- **`LINKS/*.BAT` becomes `*.COM`.** The marker batch file we ship is
  replaced at install time with an ~80 byte `.COM` launcher, so
  `LINKS/MP.BAT` in the zip arrives as `%DOSDIR%\LINKS\MP.COM`.
- **Sources are not extracted by default.** `installsources 0` is the
  FreeDOS default, so `SOURCE/MPYTHON/SOURCES.ZIP` stays in the package
  unless the user asks for it. That is correct behaviour and the reason
  shipping sources costs installed disk space only when wanted.
