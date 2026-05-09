"""freedos_micro_python - MicroPython port for FreeDOS / i386 via uc386."""

from importlib.metadata import PackageNotFoundError, version

try:
    __version__ = version("freedos_micro_python")
except PackageNotFoundError:
    __version__ = "0.0.0+unknown"
