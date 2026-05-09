#!/usr/bin/env python3
"""Unit tests for `gen_qstrdefs.py`'s reverse-mangling logic.

The smoke tests in `test_micropython_smoke.py` catch regressions
end-to-end but require a fresh ~14 min port build. These unit tests
exercise the reverse-mangler directly so a typo in
`gen_qstrdefs.py` shows up in <1s.

Three classes of regression are pinned:
- single-char escapes (`_lt_` → `<`, `_0x0a_` → `\\n`, `_space_` → ` `)
- multi-token names (`_brace_open_` → `{`, even when adjacent to
  another wrapper)
- false-match avoidance (`__and__` → `__and__`, NOT `_∧_` via the
  HTML entity `and` → U+2227)
"""
from __future__ import annotations

import importlib.util
import os
import sys
from pathlib import Path

import pytest


# gen_qstrdefs.py ships inside the package; upstream/ is fetched into
# the user's workdir by `freedos-micropython fetch`. Tests look for it
# in cwd unless FREEDOS_MP_UPSTREAM is set.
import freedos_micro_python as _fmp
_GEN_PATH = Path(_fmp.__file__).resolve().parent / "gen_qstrdefs.py"


def _upstream_dir() -> Path:
    env = os.environ.get("FREEDOS_MP_UPSTREAM")
    if env:
        return Path(env)
    return Path.cwd() / "upstream"


@pytest.fixture(scope="module")
def gen_module():
    """Same loader as `unescape`, but returns the whole module so
    callers can reach `compute_hash` too."""
    upstream_dir = _upstream_dir() / "py"
    if not (upstream_dir / "makeqstrdata.py").exists():
        pytest.skip(
            "upstream/py/makeqstrdata.py missing — run "
            "`freedos-micropython fetch` first"
        )
    spec = importlib.util.spec_from_file_location(
        "_gen_qstrdefs_under_test",
        _GEN_PATH,
    )
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    saved_cwd = Path.cwd()
    sys.path.insert(0, str(_upstream_dir().parent))
    try:
        import os
        os.chdir(_upstream_dir().parent)
        spec.loader.exec_module(mod)
    finally:
        os.chdir(saved_cwd)
    return mod


@pytest.fixture(scope="module")
def unescape():
    """Load gen_qstrdefs.py as a module (it lives next to this file
    and isn't installed as a package). Skip cleanly if upstream isn't
    fetched (the script imports `makeqstrdata` from
    `upstream/py/makeqstrdata.py` for the codepoint2name table)."""
    upstream_dir = _upstream_dir() / "py"
    if not (upstream_dir / "makeqstrdata.py").exists():
        pytest.skip(
            "upstream/py/makeqstrdata.py missing — run "
            "`freedos-micropython fetch` first"
        )
    spec = importlib.util.spec_from_file_location(
        "_gen_qstrdefs_under_test",
        _GEN_PATH,
    )
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    saved_cwd = Path.cwd()
    sys.path.insert(0, str(_upstream_dir().parent))
    try:
        # gen_qstrdefs adds 'upstream/py' to sys.path relative to cwd,
        # so run the import with cwd = _HERE.
        import os
        os.chdir(_upstream_dir().parent)
        spec.loader.exec_module(mod)
    finally:
        os.chdir(saved_cwd)
    return mod.unescape


@pytest.mark.parametrize(
    "macro_tail,expected,label",
    [
        # Single-char punctuation escapes (the main case).
        ("_lt_", "<", "lt → <"),
        ("_gt_", ">", "gt → >"),
        ("_space_", " ", "space → space"),
        ("_star_", "*", "star → *"),
        ("_colon_", ":", "colon → :"),
        ("_hash_", "#", "hash → #"),
        ("_amp_", "&", "amp → &"),
        ("_hyphen_", "-", "hyphen → -"),
        # Hex byte escapes.
        ("_0x0a_", "\n", "_0x0a_ → newline"),
        ("_0x09_", "\t", "_0x09_ → tab"),
        # Multi-token names — names with internal underscores.
        ("_brace_open_", "{", "brace_open → {"),
        ("_brace_close_", "}", "brace_close → }"),
        ("_paren_open_", "(", "paren_open → ("),
        ("_paren_close_", ")", "paren_close → )"),
        ("_bracket_open_", "[", "bracket_open → ["),
        ("_at_sign_", "@", "at_sign → @"),
        # Adjacent wrappers — `_brace_open__colon_` (the format-string
        # case from `bin()`/`hex()`/`oct()` qstrs).
        (
            "_brace_open__colon__hash_b_brace_close_",
            "{:#b}",
            "{:#b} format string",
        ),
        (
            "_brace_open__colon__hash_x_brace_close_",
            "{:#x}",
            "{:#x} format string",
        ),
        # Mix of escapes and identifier chars.
        ("_lt_stdin_gt_", "<stdin>", "<stdin>"),
        ("_lt_module_gt_", "<module>", "<module>"),
        # Pure identifier — passes through verbatim. Critical: must
        # NOT mis-decode `_and_` as U+2227 ∧.
        ("__and__", "__and__", "dunder __and__ stays literal"),
        ("__or__", "__or__", "dunder __or__ stays literal"),
        ("__not__", "__not__", "dunder __not__ stays literal"),
        ("__xor__", "__xor__", "dunder __xor__ stays literal"),
        ("__name__", "__name__", "dunder __name__ stays literal"),
        ("print", "print", "plain identifier"),
        # Mix of identifier chars + escapes.
        ("_dot_frozen", ".frozen", ".frozen"),
        # Lone underscore.
        ("_", "_", "single underscore"),
    ],
)
def test_unescape(unescape, macro_tail: str, expected: str, label: str) -> None:
    """Each `macro_tail` is what `gen_qstrdefs.py` sees after stripping
    the `MP_QSTR_` prefix. Compare its decoded payload to `expected`."""
    actual = unescape(macro_tail)
    assert actual == expected, (
        f"{label}: input {macro_tail!r} → got {actual!r}, "
        f"expected {expected!r}"
    )


def test_compute_hash_matches_upstream(gen_module) -> None:
    """`compute_hash` mirrors upstream's
    `tools/makeqstrdata.py:compute_hash` — same djb2 sequence, same
    `(hash & mask) or 1` zero-fix, same fall-back to a 16-bit mask
    when `bytes_hash == 0`. Any drift would mean
    `qstr_find_strn`'s `pool->hashes[at] == str_hash` filter rejects
    every static lookup at runtime."""
    sys.path.insert(0, str(_upstream_dir() / "py"))
    from makeqstrdata import compute_hash as upstream_hash  # type: ignore

    samples = [
        b"",
        b"__name__",
        b"print",
        b"\n",
        b"<stdin>",
        b"{:#b}",
        b"\xff",
    ]
    for bytes_hash in (0, 1, 2):
        for s in samples:
            ours = gen_module.compute_hash(s, bytes_hash)
            theirs = upstream_hash(s, bytes_hash)
            assert ours == theirs, (
                f"bytes_hash={bytes_hash} input={s!r}: "
                f"ours={ours} upstream={theirs}"
            )
            assert ours != 0, (
                f"hash-zero invariant broken for {s!r}@{bytes_hash}"
            )
