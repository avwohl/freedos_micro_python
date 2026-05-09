"""freedos-micropython CLI: builds the FreeDOS MicroPython binary.

Subcommands:
    fetch    Run scripts/fetch.sh to clone upstream MicroPython.
    build    Run scripts/build.sh (per-TU triage build, generates qstrdefs).
    port     Run scripts/build_port.sh (multi-TU build → micropython.bin).

Each subcommand is a thin wrapper around the bundled shell script. The
wrapper:

  1. Locates uc386's lib/ via the installed package and exports
     UC386_LIB_INCLUDE so the scripts don't have to compute it.
  2. Picks a working directory: uses --workdir if given, else cwd.
  3. Execs the script with stdin/stdout/stderr passed through.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from importlib.resources import files
from pathlib import Path


def _uc386_lib_include() -> Path:
    import uc386
    return Path(uc386.__file__).resolve().parent / "lib" / "include"


def _scripts_dir() -> Path:
    return Path(str(files("freedos_micro_python") / "scripts"))


def _port_dir() -> Path:
    return Path(str(files("freedos_micro_python") / "port"))


def _ensure_port_symlink(workdir: Path) -> None:
    """The shipped scripts reference port files as `uc386-dos/foo.c`.
    Make sure `workdir/uc386-dos` points at the bundled port dir so
    those references resolve regardless of where the user is running
    from."""
    link = workdir / "uc386-dos"
    target = _port_dir()
    if link.is_symlink():
        if link.resolve() == target.resolve():
            return
        link.unlink()
    elif link.exists():
        # Real directory at uc386-dos/ — leave it alone, user knows
        # what they're doing.
        return
    link.symlink_to(target)


def _run_script(name: str, workdir: Path, extra_args: list[str]) -> int:
    script = _scripts_dir() / name
    if not script.exists():
        print(f"freedos-micropython: bundled script not found: {script}",
              file=sys.stderr)
        return 1
    env = os.environ.copy()
    env["UC386_LIB_INCLUDE"] = str(_uc386_lib_include())
    env["FREEDOS_MP_PORT_DIR"] = str(_port_dir())
    env["FREEDOS_MP_SCRIPTS_DIR"] = str(_scripts_dir())
    workdir.mkdir(parents=True, exist_ok=True)
    _ensure_port_symlink(workdir)
    return subprocess.run(
        [str(script), *extra_args],
        cwd=workdir, env=env,
    ).returncode


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="freedos-micropython",
        description=(
            "Build the FreeDOS / i386 MicroPython port end-to-end "
            "through the uc386 C23 compiler. Produces a flat .bin "
            "runnable under uc386.dos_emu (or a PMODE/W .exe via "
            "uc386's exe.py harness)."
        ),
    )
    ap.add_argument(
        "--workdir", type=Path, default=Path.cwd(),
        help="Directory the build runs in (upstream/ and build/ "
             "land here). Default: current dir.",
    )
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("fetch", help="Clone upstream micropython into ./upstream")
    sub.add_parser("build", help="Per-TU triage build (generates qstrdefs)")
    sub.add_parser("port",  help="Multi-TU build → build/micropython.bin")

    ns, extra = ap.parse_known_args(argv)
    script_map = {
        "fetch": "fetch.sh",
        "build": "build.sh",
        "port":  "build_port.sh",
    }
    return _run_script(script_map[ns.cmd], ns.workdir, extra)


if __name__ == "__main__":
    raise SystemExit(main())
