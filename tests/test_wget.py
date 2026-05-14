"""Smoke tests for examples/wget.py running under uc386-dos MicroPython.

Two layers:

1. Host-side parser tests — exercise `wget._split_url`,
   `wget._resolve_redirect`, and `wget._build_request` under the
   bog-standard CPython interpreter. These pin the URL parser
   and HTTP request shape without depending on MP being built.

2. Embedded MP test — pastes the wget.py source into the
   uc386-dos MicroPython REPL via dos_emu, then runs the same
   parser probes. Pins that wget.py's imports (`socket`, `ssl`,
   `urllib.parse` via `urllib_parse` fallback) resolve under the
   port and that the parser produces identical results on both
   sides of the C frontier.

End-to-end (`wget.fetch` against a live HTTPS server) needs a
real TCP path and isn't exercised here — dos_emu's
NetworkSimulator doesn't bridge to host sockets, and the
wire-level qemu rig still needs an MP.EXE built via Watcom
wlink (Linux/Windows only). See `rigs/tls-rig/run-tls-rig.sh`
for the wire-level path.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
_WGET = _HERE.parent / "examples" / "wget.py"


def _bin_candidates() -> list[Path]:
    """Same lookup `test_smoke.py` uses — env var + a couple of
    canonical build-output paths."""
    import os
    cands: list[Path] = []
    env = os.environ.get("FREEDOS_MP_BIN")
    if env:
        cands.append(Path(env))
    cwd = Path.cwd()
    cands.extend([
        cwd / "build" / "micropython.bin",
        cwd / "micropython.bin",
        _HERE / "build" / "micropython.bin",
    ])
    return cands


def _find_bin() -> Path | None:
    for p in _bin_candidates():
        if p.exists():
            return p
    return None


@pytest.fixture(scope="module")
def micropython_bin() -> Path:
    p = _find_bin()
    if p is None:
        pytest.skip(
            "micropython.bin not built — run `freedos-micropython port` "
            "first or set FREEDOS_MP_BIN"
        )
    return p


@pytest.fixture(scope="module")
def wget_module():
    """Load examples/wget.py as a host-side Python module so we can
    poke at its private helpers."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("wget", _WGET)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ---- host-side parser tests --------------------------------------


def test_split_url_https_default_port(wget_module):
    """Implicit https → port 443. Path + query merge into a single
    request-line target."""
    assert wget_module._split_url(
        "https://example.com/foo/bar?x=1"
    ) == ("https", "example.com", 443, "/foo/bar?x=1")


def test_split_url_http_explicit_port(wget_module):
    """Explicit port survives the split. Default scheme stays http."""
    assert wget_module._split_url(
        "http://example.com:8080/"
    ) == ("http", "example.com", 8080, "/")


def test_split_url_strips_userinfo(wget_module):
    """``user:pass@host`` userinfo is dropped (per HTTP spec, not
    sent in the request)."""
    scheme, host, port, path = wget_module._split_url(
        "https://user:pass@example.com/path"
    )
    assert host == "example.com" and port == 443 and path == "/path"


def test_resolve_redirect_absolute(wget_module):
    """Abs-URL Location is returned verbatim."""
    assert wget_module._resolve_redirect(
        "https", "a.com", "/p", "http://b.com/x"
    ) == "http://b.com/x"


def test_resolve_redirect_abs_path(wget_module):
    """``/foo`` Location resolves against scheme+host."""
    assert wget_module._resolve_redirect(
        "https", "a.com", "/p", "/q"
    ) == "https://a.com/q"


def test_resolve_redirect_relative(wget_module):
    """Bare-relative Location resolves against the request's
    parent directory (everything up to the last `/`)."""
    assert wget_module._resolve_redirect(
        "https", "a.com", "/p/x", "y"
    ) == "https://a.com/p/y"


def test_build_request_has_required_headers(wget_module):
    """The minimal HTTP/1.0 request includes Host (mandatory in
    multi-vhost servers) and Connection: close (so the server
    closes the stream when done — wget reads until EOF)."""
    req = wget_module._build_request("example.com", "/foo", None)
    assert req.startswith(b"GET /foo HTTP/1.0\r\n")
    assert b"\r\nHost: example.com\r\n" in req
    assert b"\r\nConnection: close\r\n" in req
    assert req.endswith(b"\r\n\r\n")


def test_build_request_custom_headers(wget_module):
    """Caller-supplied headers append to the base set."""
    req = wget_module._build_request(
        "example.com", "/",
        {"Authorization": "Bearer xyz"},
    )
    assert b"\r\nAuthorization: Bearer xyz\r\n" in req


# ---- embedded MicroPython smoke ----------------------------------


def test_wget_runs_under_micropython(micropython_bin: Path) -> None:
    """Paste wget.py into the uc386-dos REPL and run the same
    parser probes. Confirms `import socket`, `import ssl`, and the
    `urllib.parse` → `urllib_parse` fallback all resolve, and the
    URL/redirect/request helpers produce the same outputs as on
    host CPython."""
    from uc386.harness import run

    wget_src = _WGET.read_text()
    # The `if __name__ == "__main__":` runner calls main() with no
    # args — paste-mode would trip its argv-required error path.
    wget_src = wget_src.replace(
        'if __name__ == "__main__":\n    sys.exit(main())\n', "",
    )

    probes = (
        'print("split:", _split_url("https://example.com/foo?x=1"))\n'
        'print("port:", _split_url("http://example.com:8080/"))\n'
        'print("redir-abs:", _resolve_redirect("https","a.com","/p","/q"))\n'
        'print("redir-rel:", _resolve_redirect("https","a.com","/p/x","y"))\n'
        'req = _build_request("example.com", "/foo", None)\n'
        'print("has_host:", b"Host: example.com" in req)\n'
        'print("has_close:", b"Connection: close" in req)\n'
        'print("done")\n'
    )

    # Ctrl-E enters paste mode; Ctrl-D exits and triggers execution.
    # Two paste blocks: load wget, then run the probes (which have
    # the helpers in scope from the first paste).
    stdin_bytes = (
        b"\x05" + wget_src.encode("utf-8") + b"\x04\n"
        b"\x05" + probes.encode("utf-8") + b"\x04\n"
        b"\x04"
    )

    res = run(
        micropython_bin,
        stdin_bytes=stdin_bytes,
        timeout_seconds=30.0,
        instruction_limit=20_000_000_000,
    )
    assert res.error is None, f"dos_emu error: {res.error}"
    assert "split: ('https', 'example.com', 443, '/foo?x=1')" in res.stdout
    assert "port: ('http', 'example.com', 8080, '/')" in res.stdout
    assert "redir-abs: https://a.com/q" in res.stdout
    assert "redir-rel: https://a.com/p/y" in res.stdout
    assert "has_host: True" in res.stdout
    assert "has_close: True" in res.stdout
    assert "done" in res.stdout
