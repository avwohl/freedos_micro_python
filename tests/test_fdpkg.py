#!/usr/bin/env python3
"""Conformance tests for the FreeDOS package built by release/mkfdpkg.py.

These assert the rules the FreeDOS repository actually enforces, which
are stricter (and in a couple of places different) than the published
spec at https://help.freedos.org/docs/info/package.html. Each rule
below was verified in August 2026 against real packages pulled from

    https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/latest/

The test builds a package from a stub executable rather than requiring
a real MP.EXE, so it runs without a port build. See
docs/FREEDOS_PACKAGING.md for the format writeup.

Usage:
    pytest tests/test_fdpkg.py
"""
from __future__ import annotations

import gzip
import importlib.util
import io
import zipfile
import zlib
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
MKFDPKG = REPO_ROOT / "release" / "mkfdpkg.py"


def _load_mkfdpkg():
    if not MKFDPKG.is_file():
        pytest.skip(f"{MKFDPKG} not present")
    spec = importlib.util.spec_from_file_location("mkfdpkg", MKFDPKG)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def mk():
    return _load_mkfdpkg()


@pytest.fixture(scope="module")
def pkg_bytes(mk, tmp_path_factory):
    """A real package, built from a stub executable."""
    stub = tmp_path_factory.mktemp("exe") / "MP.EXE"
    stub.write_bytes(b"MZ" + b"\0" * 4094)
    return mk.build_package(
        stub, version="0.0.0-test", entered="2026-08-13",
        modified="2026-08-13.0", description="Python 3 interpreter and REPL",
        summary="Test build.", uc386_version="0.0.0-test",
    )


@pytest.fixture(scope="module")
def zf(pkg_bytes):
    return zipfile.ZipFile(io.BytesIO(pkg_bytes))


# --------------------------------------------------------------------------
# zip-level rules
# --------------------------------------------------------------------------

def test_every_path_fits_8_3(mk, zf):
    """FAT without LFN support silently mangles anything longer."""
    for name in zf.namelist():
        mk.check_83(name)  # raises SystemExit on violation


def test_nothing_at_the_zip_root(zf):
    """FDRepo rejects packages with files in the root directory: they
    cause filename collisions when several packages are installed."""
    root_files = [n for n in zf.namelist() if "/" not in n.rstrip("/")]
    assert root_files == [], f"files at zip root: {root_files}"


def test_deflate_only(zf):
    """FDNPKG also supports LZMA, but unpacking it needs ~24 MiB — more
    than most DOS machines have. Deflate is the safe choice."""
    for info in zf.infolist():
        assert info.compress_type == zipfile.ZIP_DEFLATED, (
            f"{info.filename} uses compress_type {info.compress_type}")


def test_expected_layout(mk, zf):
    names = set(zf.namelist())
    pkgu = mk.PKG.upper()
    for required in (
        f"APPINFO/{pkgu}.LSM",
        f"{mk.CATEGORY_DIR}/{pkgu}/MP.EXE",
        f"{mk.CATEGORY_DIR}/{pkgu}/README.TXT",
        f"{mk.CATEGORY_DIR}/{pkgu}/LICENSE.TXT",
        "LINKS/MP.BAT",
        f"SOURCE/{pkgu}/SOURCES.ZIP",
    ):
        assert required in names, f"missing {required}"


def test_package_name_is_legal(mk):
    """<= 8 chars from [a-z0-9_], because it is also the 8.3 filename."""
    assert 1 <= len(mk.PKG) <= 8
    assert all(c.islower() or c.isdigit() or c == "_" for c in mk.PKG)


# --------------------------------------------------------------------------
# LSM
# --------------------------------------------------------------------------

def test_lsm_is_well_formed(mk, zf):
    text = zf.read(f"APPINFO/{mk.PKG.upper()}.LSM").decode("cp437")
    lines = text.splitlines()
    assert lines[0] == "Begin3"
    assert lines[-1] == "End"

    fields = {}
    for line in lines[1:-1]:
        if line.startswith(" "):
            continue  # continuation
        key, _, value = line.partition(":")
        fields[key.strip()] = value.strip()

    # Required by the spec.
    for required in ("Title", "Version", "Entered-Date", "Description",
                     "Summary", "Author", "Copying-Policy"):
        assert required in fields, f"LSM missing required field {required}"
        assert fields[required], f"LSM field {required} is empty"

    # The spec says "full unabridged name of the license" — an
    # abbreviation like "MIT" or "GPLv2" is what it explicitly forbids.
    policy = fields["Copying-Policy"]
    assert policy not in ("MIT", "GPLv2", "BSD", "GPL"), (
        f"Copying-Policy must be unabridged, got {policy!r}")

    # Real packages spell the maintainer field this way.
    assert "Maintained-By" in fields


def test_lsm_continuations_are_indented(mk, zf):
    """FDRepo parses continuations by leading whitespace, so a wrapped
    value that starts at column 0 would be read as a new field."""
    text = zf.read(f"APPINFO/{mk.PKG.upper()}.LSM").decode("cp437")
    for line in text.splitlines()[1:-1]:
        if not line.startswith(" "):
            assert ":" in line, f"unindented non-field line: {line!r}"


# --------------------------------------------------------------------------
# LINKS
# --------------------------------------------------------------------------

def test_link_files_point_at_the_executable(mk, zf):
    """A LINKS/*.BAT is a marker holding only the in-zip path of its
    target; the package manager rewrites it at install time. The
    official packages write these LF-only, so match that — FDNPKG
    parses the path out of the file and a stray CR would corrupt it."""
    for name in zf.namelist():
        if not name.startswith("LINKS/"):
            continue
        raw = zf.read(name)
        assert b"\r" not in raw, f"{name} must be LF-only"
        target = raw.decode("ascii").strip()
        assert target == f"{mk.CATEGORY_DIR.lower()}\\{mk.PKG}\\mp.exe"


# --------------------------------------------------------------------------
# sources
# --------------------------------------------------------------------------

def test_sources_zip_carries_the_port(mk, zf):
    """Source inclusion is expected by the package spec, and this
    project ships it regardless (see release/README.md)."""
    inner = zipfile.ZipFile(io.BytesIO(
        zf.read(f"SOURCE/{mk.PKG.upper()}/SOURCES.ZIP")))
    names = inner.namelist()
    assert "BUILD.txt" in names
    assert "LICENSE" in names
    assert any(n.startswith("src/freedos_micro_python/port/") for n in names)
    assert any(n.endswith("mpconfigport.h") for n in names)


def test_build_txt_pins_every_upstream(mk, zf):
    inner = zipfile.ZipFile(io.BytesIO(
        zf.read(f"SOURCE/{mk.PKG.upper()}/SOURCES.ZIP")))
    build_txt = inner.read("BUILD.txt").decode()
    for name, (url, ref) in mk.PINS.items():
        assert url in build_txt, f"BUILD.txt missing {name} url"
        assert ref in build_txt, f"BUILD.txt missing {name} pin {ref}"


# --------------------------------------------------------------------------
# repository index
# --------------------------------------------------------------------------

def test_index_lst_format(mk, pkg_bytes, tmp_path):
    """Verified against the live devel/index.lst:

        FD-REPOv1<TAB>Build time: <epoch><TAB><group><TAB><count>CRLF
        <pkg><TAB><version><TAB><description><TAB><CRC32>CRLF
    """
    crc = "%08X" % (zlib.crc32(pkg_bytes) & 0xFFFFFFFF)
    row = {
        "package": mk.PKG, "title": "MicroPython", "group": mk.GROUP,
        "modified-date": "2026-08-13.0", "entered-date": "2026-08-13",
        "version": "0.0.0-test", "author": "a", "copying-policy": "b",
        "crc32": crc, "description": "Python 3 interpreter and REPL",
        "summary": "s", "keywords": "k",
    }
    mk.build_repo(tmp_path, pkg_bytes, row, mk.GROUP, "http://example.org/repo")

    raw = (tmp_path / "repo" / mk.GROUP / "index.lst").read_bytes()
    assert raw.endswith(b"\r\n")
    assert b"\n" not in raw.replace(b"\r\n", b"")   # CRLF only, no bare LF

    lines = raw.decode("cp437").split("\r\n")[:-1]
    header = lines[0].split("\t")
    assert header[0] == "FD-REPOv1"
    assert header[1].startswith("Build time: ")
    assert header[1][len("Build time: "):].isdigit()
    assert header[2] == mk.GROUP
    assert int(header[3]) == len(lines) - 1

    pkg, version, description, index_crc = lines[1].split("\t")
    assert pkg == mk.PKG
    assert version == "0.0.0-test"
    # The index CRC is the CRC-32 of the package zip itself — verified
    # against bwbasic.zip, which hashes to the C48ADF75 its row claims.
    assert index_crc == crc


def test_index_gz_matches_index_lst(mk, pkg_bytes, tmp_path):
    row = {
        "package": mk.PKG, "group": mk.GROUP, "version": "0.0.0-test",
        "description": "d", "crc32": "00000000",
    }
    mk.build_repo(tmp_path, pkg_bytes, row, mk.GROUP, "http://example.org/repo")
    gdir = tmp_path / "repo" / mk.GROUP
    assert gzip.decompress((gdir / "index.gz").read_bytes()) == \
        (gdir / "index.lst").read_bytes()


def test_repo_ships_the_package_and_listing(mk, pkg_bytes, tmp_path):
    row = {
        "package": mk.PKG, "group": mk.GROUP, "version": "0.0.0-test",
        "description": "d", "crc32": "00000000",
    }
    mk.build_repo(tmp_path, pkg_bytes, row, mk.GROUP, "http://example.org/repo")
    repo = tmp_path / "repo"
    assert (repo / mk.GROUP / f"{mk.PKG}.zip").read_bytes() == pkg_bytes
    listing = (repo / "listing.csv").read_text()
    assert listing.splitlines()[0].startswith("package,title,group,")
    # One REPO line per group directory is how FDNPKG is configured.
    assert "REPO http://example.org/repo/devel" in \
        (repo / "FDNPKG.CFG.SAMPLE").read_text()


# --------------------------------------------------------------------------
# the 8.3 checker itself
# --------------------------------------------------------------------------

@pytest.mark.parametrize("bad", [
    "APPINFO/TOOLONGNAME.LSM",
    "DEVEL/MPYTHON/micropython.bin",
    "DEVEL/VERYLONGDIR/MP.EXE",
    "DEVEL/MPYTHON/MP.EXECUTABLE",
])
def test_check_83_rejects(mk, bad):
    with pytest.raises(SystemExit):
        mk.check_83(bad)


@pytest.mark.parametrize("good", [
    "APPINFO/MPYTHON.LSM",
    "DEVEL/MPYTHON/MP.EXE",
    "SOURCE/MPYTHON/SOURCES.ZIP",
    "LINKS/MP.BAT",
])
def test_check_83_accepts(mk, good):
    mk.check_83(good)
