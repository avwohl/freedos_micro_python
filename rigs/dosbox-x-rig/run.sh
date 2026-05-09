#!/bin/sh
# Launch DOSBox-X with the rig config. Headless if `nogui` is
# requested (set DOSBOX_HEADLESS=1) — useful for CI; otherwise
# pops a window so you can interact with the REPL.
set -eu
cd "$(dirname "$0")"

if ! command -v dosbox-x >/dev/null 2>&1; then
    echo "dosbox-x not on PATH. Install with:" >&2
    echo "  brew install dosbox-x   # macOS" >&2
    echo "  apt install dosbox-x    # Debian/Ubuntu" >&2
    exit 1
fi

# Warn rather than fail when MP.EXE is absent — autoexec.bat will
# print a helpful message inside the DOS session.
if [ ! -f MP.EXE ] && [ ! -f mp.exe ]; then
    echo "warn: MP.EXE not in $(pwd) — see README.md to build it." >&2
fi
if [ ! -f NE2000.COM ] && [ ! -f ne2000.com ]; then
    echo "warn: NE2000.COM not in $(pwd) — fetch.sh will get one." >&2
fi

# Clear the previous run's log so callers see only the latest.
rm -f RIG.LOG

# `-silent` runs autoexec.bat with no DOSBox-X window, no SDL
# audio init, then exits. autoexec.bat tees its output into
# RIG.LOG (regular DOS redirect) so we can read the result.
dosbox-x -silent -conf dosbox-x.conf >/dev/null 2>&1 || true

if [ -f RIG.LOG ]; then
    cat RIG.LOG
else
    echo "rig: DOSBox-X exited without writing RIG.LOG" >&2
    exit 1
fi
