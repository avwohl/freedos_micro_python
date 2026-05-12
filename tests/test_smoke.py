#!/usr/bin/env python3
"""Smoke test for the uc386-built MicroPython binary.

Runs `addons/gnu/micropython/build/micropython.bin` under
`uc386.dos_emu` and asserts it boots far enough to print the REPL
banner and the `>>> ` prompt.

Skipped when `micropython.bin` doesn't exist — the binary is a
derived artifact (built by `addons/gnu/micropython/build_port.sh`,
~14 min wall-clock); we don't check it in. Run after a port build
to guard against regressions.

The REPL banner format is fixed by `shared/runtime/pyexec.c`'s
`pyexec_friendly_repl_print_banner`:

    MicroPython <git-tag> on <build-date>; <hw-board> with <hw-mcu>
    Type "help()" for more information.
    >>>

We assert the prefix only (the date varies per build).

Usage:
    .venv/bin/pytest addons/gnu/micropython/test_micropython_smoke.py
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest


# Locate micropython.bin. After `freedos-micropython port` lands in
# <workdir>/build/micropython.bin. The env var lets CI / explicit
# invocations override.
def _bin_candidates() -> list[Path]:
    env = os.environ.get("FREEDOS_MP_BIN")
    cands = [Path(env)] if env else []
    cwd = Path.cwd()
    cands.extend([
        cwd / "build" / "micropython.bin",
        cwd / "micropython.bin",
        Path(__file__).resolve().parent / "build" / "micropython.bin",
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
            "micropython.bin not built — run "
            "`freedos-micropython port` first (~14 min) or set "
            "FREEDOS_MP_BIN to an existing binary"
        )
    return p


def test_micropython_repl_banner(micropython_bin: Path) -> None:
    """MicroPython should boot far enough to print the REPL banner
    and the `>>> ` prompt, then wait on stdin."""
    from uc386.harness import run

    res = run(micropython_bin,
              timeout_seconds=10.0,
              instruction_limit=2_000_000_000)
    # The REPL waits on stdin and dos_emu hits its timeout — that's
    # the expected steady state. error=None means no faults during
    # boot.
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert "MicroPython" in res.stdout, (
        f"REPL banner missing from stdout: {res.stdout!r}"
    )
    assert "uc386-dos with i386" in res.stdout, (
        f"port banner ('uc386-dos with i386') missing: "
        f"{res.stdout!r}"
    )
    assert ">>> " in res.stdout, (
        f"REPL prompt ('>>> ') missing: {res.stdout!r}"
    )


def test_micropython_named_builtin(micropython_bin: Path) -> None:
    """`__name__\\n\\x04` exercises the static-qstr LOAD_GLOBAL path:
    mp_init's dict_main store of `__name__` (qstr id 67) had to
    survive the qstr_find_strn binary search through all 878
    main-pool entries. Required: LC_ALL=C ASCII collation in the
    qstrdefs sort + real strlen in the QDEF length field. Result
    should print the module name `'__main__'`."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"__name__\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "'__main__'" in res.stdout, (
        f"expected `'__main__'` in stdout, got: {res.stdout!r}"
    )


def test_micropython_arithmetic(micropython_bin: Path) -> None:
    """`2+3\\n\\x04` exercises the value-print path: lex → parse →
    compile → VM dispatch (LOAD_CONST 2; LOAD_CONST 3; BINARY_OP +;
    __repl_print__ wrap) → mp_load_global → builtins dict lookup →
    print result. Until commit 19ae598 this trapped in
    `_pass_push_memory_to_push_reg`, which was incorrectly merging
    chained pointer dereferences (`mov eax, [eax+4]; push [eax+4]`
    → `push eax`)."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"2+3\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0, (
        f"unexpected exit code: {res.exit_code} "
        f"(expected 0 for `2+3\\n\\x04` clean exit)"
    )
    # Result `5` should appear on its own line between the two
    # `>>> ` prompts. Use a substring check so prompt-formatting
    # tweaks don't break the test.
    assert "\n5\n" in res.stdout, (
        f"expected arithmetic result `5` in stdout, got: {res.stdout!r}"
    )


def test_micropython_assign_statement(micropython_bin: Path) -> None:
    """`x = 5\\n\\x04` exercises the qstr-store path that the
    QDEF1 fix unblocked: lexes the identifier, allocates an
    interned qstr, dict-stores 5 against it, then EOF exits.

    Different from `pass` — that one didn't allocate any qstrs.
    Until commit 21dc0d9 this trapped in `qstr_find_strn`'s
    binary search because the empty-main-pool corner case
    underflowed the high-bound."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"x = 5\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0, (
        f"unexpected exit code: {res.exit_code} "
        f"(expected 0 for `x = 5\\n\\x04` clean exit)"
    )
    assert res.stdout.count(">>> ") >= 2, (
        f"expected two `>>> ` prompts in stdout, got: {res.stdout!r}"
    )


def test_micropython_pass_statement(micropython_bin: Path) -> None:
    """Sending `pass\\n\\x04` exercises the full lex → parse → compile
    → exec path: `pass` is a no-op statement, so the REPL shouldn't
    print anything but the next `>>> ` prompt before EOF exits.

    This pins the parser + compiler + VM + NLR (exception machinery)
    after the REPL banner. Expression statements that produce a value
    (`1`, `print(2+3)`) currently fail in the value-print path —
    that's a separate gap, tracked in NOTES.md."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"pass\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0, (
        f"unexpected exit code: {res.exit_code} "
        f"(expected 0 for `pass\\n\\x04` clean exit)"
    )
    # After `pass` executes (no output), REPL prints `>>> ` again,
    # then Ctrl-D exits. Two prompts is the diagnostic.
    assert res.stdout.count(">>> ") >= 2, (
        f"expected two `>>> ` prompts in stdout (one before `pass`, "
        f"one after), got: {res.stdout!r}"
    )


def test_micropython_clean_eof_exit(micropython_bin: Path) -> None:
    """Sending only Ctrl-D (EOF) at the prompt should exit the REPL
    cleanly with exit code 0 — exercises the readline → pyexec EOF
    path that runs after the boot banner."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"\x04",
              timeout_seconds=15.0,
              instruction_limit=2_000_000_000)
    assert not res.timed_out, "REPL didn't exit on Ctrl-D"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0, (
        f"unexpected exit code: {res.exit_code} "
        f"(expected 0 for clean Ctrl-D exit)"
    )
    assert ">>> " in res.stdout, (
        f"REPL prompt missing before Ctrl-D was processed: "
        f"{res.stdout!r}"
    )


def test_micropython_print_real_newline(micropython_bin: Path) -> None:
    """`print(2+3)\\n\\x04` exercises the qstr-payload path: the
    builtin `print()` ends each line with `\\n`, which is qstr id
    `MP_QSTR__0x0a_`. Until commit 2abb610 the qstrdefs generator
    captured the *sanitized* macro tail as the qstr's payload string,
    so `print()` emitted the literal text `_0x0a_` instead of a
    newline. Fix: gen_qstrdefs.py reverses upstream's qstr_escape
    (re-using upstream's codepoint2name map) so escaped qstrs ship
    their original byte string in the QDEF1 payload."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"print(2+3)\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    # Result `5` followed by a real `\n`, then the next `>>> ` prompt.
    # The bug rendered this as `5_0x0a_>>>` (concatenated, no newline).
    assert "5\n>>> " in res.stdout, (
        f"expected `5` + real newline + prompt in stdout, got: "
        f"{res.stdout!r}"
    )
    # Belt-and-suspenders: explicitly forbid the pre-fix mangled form.
    assert "_0x0a_" not in res.stdout, (
        f"found mangled `_0x0a_` (qstr_escape not reversed): "
        f"{res.stdout!r}"
    )


def test_micropython_def_and_call(micropython_bin: Path) -> None:
    """Exercise function definition + call: `def f(x): return x*2`,
    then `f(7)` → `14`. Pins compile of FunctionDef and the
    bytecode CALL_FUNCTION → MAKE_FUNCTION → arg binding → return
    path."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"def f(x): return x*2\n\nprint(f(7))\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "\n14\n" in res.stdout, (
        f"expected `f(7) = 14` in stdout, got: {res.stdout!r}"
    )


def test_micropython_list_comprehension(micropython_bin: Path) -> None:
    """`[i*i for i in range(5)]` exercises the comprehension scope
    + generator + range iter path, then prints the list. Pins
    objlist + objgenerator + objrange + the BUILD_LIST opcode."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"print([i*i for i in range(5)])\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "[0, 1, 4, 9, 16]" in res.stdout, (
        f"expected list-comprehension result in stdout, got: "
        f"{res.stdout!r}"
    )


def test_micropython_bin_hex_oct(micropython_bin: Path) -> None:
    """`bin(10)` → `0b1010`, `hex(255)` → `0xff`, `oct(8)` → `0o10`.
    These exercise qstrs whose escape names CONTAIN underscores
    (`brace_open`, `brace_close`, `colon`, `hash`) — the reverse-
    mangler must not split `_brace_open_` on the inner `_` and
    candidate-match `brace`. Pre-fix output was the macro-mangled
    text: `_brace_open_:#b_brace_close_` instead of `0b1010`."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"print(bin(10))\n"
                  b"print(hex(255))\n"
                  b"print(oct(8))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "0b1010" in res.stdout, (
        f"expected `bin(10)` → `0b1010`, got: {res.stdout!r}"
    )
    assert "0xff" in res.stdout, (
        f"expected `hex(255)` → `0xff`, got: {res.stdout!r}"
    )
    assert "0o10" in res.stdout, (
        f"expected `oct(8)` → `0o10`, got: {res.stdout!r}"
    )
    # Belt-and-suspenders against regression: the macro-mangled form
    # `_brace_open_` should never appear in output.
    assert "_brace_open_" not in res.stdout, (
        f"qstr macro-mangling leaked: {res.stdout!r}"
    )


def test_micropython_enumerate_filter_property(micropython_bin: Path) -> None:
    """`enumerate`, `filter`, and `@property` are the next batch of
    CORE_FEATURES-gated builtins the port opts into selectively (
    MICROPY_PY_BUILTINS_ENUMERATE / FILTER / PROPERTY in
    mpconfigport.h, while staying at ROM_LEVEL_MINIMUM). Each pulls
    in self-contained .c that's already in the multi-TU compile."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"print(list(enumerate(['a','b','c'])))\n"
                  b"print(list(filter(lambda x: x>2, [1,2,3,4])))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "[(0, 'a'), (1, 'b'), (2, 'c')]" in res.stdout, (
        f"expected enumerate output, got: {res.stdout!r}"
    )
    assert "[3, 4]" in res.stdout, (
        f"expected filter output `[3, 4]`, got: {res.stdout!r}"
    )


def test_micropython_property_decorator(micropython_bin: Path) -> None:
    """`@property` decorator wraps an instance method as a read-only
    attribute. Pins the descriptor protocol + decorator dispatch."""
    from uc386.harness import run

    res = run(
        micropython_bin,
        stdin_bytes=(
            b"class C:\n"
            b"    @property\n"
            b"    def x(self): return 42\n"
            b"\n"
            b"print(C().x)\n"
            b"\x04"
        ),
        timeout_seconds=15.0,
        instruction_limit=4_000_000_000,
    )
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "\n42\n" in res.stdout, (
        f"expected `C().x` → `42`, got: {res.stdout!r}"
    )


def test_micropython_min_max_reversed(micropython_bin: Path) -> None:
    """`min([3,1,2])`, `max([3,1,2])`, `reversed([1,2,3])` exercise
    the CORE_FEATURES-gated builtins that the port now opts into
    selectively (MICROPY_PY_BUILTINS_MIN_MAX, MICROPY_PY_BUILTINS_REVERSED
    in mpconfigport.h, while staying at ROM_LEVEL_MINIMUM).

    Pinned because they're easy to lose: a future rebuild that
    forgets the opt-ins would silently lose `min`/`max`/`reversed`
    without breaking any other test."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"print(min([3,1,2]))\n"
                  b"print(max([3,1,2]))\n"
                  b"print(list(reversed([1,2,3])))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "\n1\n" in res.stdout, (
        f"expected `min` result `1` in stdout, got: {res.stdout!r}"
    )
    assert "\n3\n" in res.stdout, (
        f"expected `max` result `3` in stdout, got: {res.stdout!r}"
    )
    assert "[3, 2, 1]" in res.stdout, (
        f"expected `reversed` result `[3, 2, 1]` in stdout, got: "
        f"{res.stdout!r}"
    )


def test_micropython_try_except(micropython_bin: Path) -> None:
    """`try: 1/0 except ZeroDivisionError: print(\"caught\")`
    exercises the NLR (non-local return) path: the VM raises
    ZeroDivisionError, the SETUP_EXCEPT handler frame catches it,
    matches the type, runs the handler. setjmp-backed NLR
    (MICROPY_NLR_SETJMP=1 in mpconfigport.h) is what makes this
    work — uc386 can't compile nlrx86.c's inline asm."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"try:\n    1/0\nexcept:\n    print('caught')\n\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "caught" in res.stdout, (
        f"expected `caught` in stdout (try/except didn't work): "
        f"{res.stdout!r}"
    )


def test_micropython_core_features_bytearray(micropython_bin: Path) -> None:
    """`bytearray(b'abc')` is gated at CORE_FEATURES (default-off at
    MINIMUM via MICROPY_PY_BUILTINS_BYTEARRAY). Pins the type as
    runnable end-to-end on the uc386-built bin."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"print(bytearray(b'abc'))\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "bytearray(b'abc')" in res.stdout, (
        f"expected bytearray repr, got: {res.stdout!r}"
    )


def test_micropython_core_features_set(micropython_bin: Path) -> None:
    """`set` literal + binary `|` exercises CORE_FEATURES gates
    (MICROPY_PY_BUILTINS_SET). The dedup output `{1, 2, 3}` from
    `set([1,2,2,3])` proves objset.c's hash-based de-duplication is
    wired correctly across the multi-TU build."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"print(set([1,2,2,3]))\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "{1, 2, 3}" in res.stdout, (
        f"expected set output `{{1, 2, 3}}`, got: {res.stdout!r}"
    )


def test_micropython_core_features_named_error(micropython_bin: Path) -> None:
    """At CORE_FEATURES `MICROPY_ERROR_REPORTING_DETAILED` includes
    the offending qstr name in NameError messages (vs MINIMUM's
    `name not defined` placeholder). Pin the rich form so a future
    ROM-level downgrade is caught loudly."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"print(undefined_name)\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "undefined_name" in res.stdout, (
        f"expected qstr name in NameError, got: {res.stdout!r}"
    )
    assert "NameError" in res.stdout, (
        f"expected NameError, got: {res.stdout!r}"
    )


def test_micropython_core_features_str_modulo(micropython_bin: Path) -> None:
    """C-style `%` string formatting (`'%d-%s' % (5, 'x')`) is gated
    on `MICROPY_PY_BUILTINS_STR_OP_MODULO` which default-enables at
    CORE_FEATURES. Pins the formatter end-to-end."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"print('%d-%s' % (5, 'x'))\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "5-x" in res.stdout, (
        f"expected `5-x` from str %% formatting, got: {res.stdout!r}"
    )


def test_micropython_import_sys(micropython_bin: Path) -> None:
    """`import sys` exercises the module-table lookup path. Until
    moduledefs.h registered `mp_module_sys`, this raised
    `ImportError: no module named 'sys'`. Pins both the registration
    + the sys module's static-init."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"import sys\nprint(sys.implementation.name)\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "micropython" in res.stdout, (
        f"expected `micropython` from sys.implementation.name, got: "
        f"{res.stdout!r}"
    )


def test_micropython_import_gc(micropython_bin: Path) -> None:
    """`import gc; gc.collect()` exercises the gc module + its
    `gc.collect` entry. Gated on `MICROPY_PY_GC` which default-on
    at CORE_FEATURES."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"import gc\ngc.collect()\nprint('ok')\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "ok" in res.stdout, (
        f"expected `ok` after gc.collect(), got: {res.stdout!r}"
    )


def test_micropython_import_collections(micropython_bin: Path) -> None:
    """`import collections; OrderedDict` exercises the collections
    module — gated on `MICROPY_PY_COLLECTIONS` (CORE_FEATURES)
    and a uc386-side moduledefs.h registration."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"from collections import OrderedDict\n"
                  b"d = OrderedDict()\n"
                  b"d['a'] = 1\n"
                  b"d['b'] = 2\n"
                  b"print(list(d.keys()))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "['a', 'b']" in res.stdout, (
        f"expected ordered keys `['a', 'b']`, got: {res.stdout!r}"
    )


def test_micropython_import_struct(micropython_bin: Path) -> None:
    """`import struct; struct.pack` exercises struct module — gated
    on `MICROPY_PY_STRUCT` (CORE_FEATURES). Pinned because struct's
    little-endian byte layout is sensitive to misaligned codegen."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import struct\n"
                  b"print(struct.pack('<I', 0x12345678))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    # 0x12345678 little-endian = 78 56 34 12
    assert "b'xV4\\x12'" in res.stdout, (
        f"expected packed bytes `b'xV4\\\\x12'`, got: {res.stdout!r}"
    )


def test_micropython_import_errno(micropython_bin: Path) -> None:
    """`import errno; errno.EINVAL` exercises the errno module —
    pulled in via explicit `MICROPY_PY_ERRNO=1` opt-in. The X-macro
    qstrs (`MP_QSTR_EPERM`, `MP_QSTR_EINVAL`, etc.) are pre-emitted
    into qstrdefs.generated.h by build.sh's X-macro-aware grep —
    without that, uc386 fails compile with `float init must be a
    constant expression (got Identifier)` because the qstrs aren't
    enum constants the static-init can resolve. Pin so a future
    config change doesn't silently drop the surface."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"import errno\nprint(errno.EINVAL)\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "22" in res.stdout, (
        f"expected EINVAL=22 from errno module, got: {res.stdout!r}"
    )


def test_micropython_import_math(micropython_bin: Path) -> None:
    """`import math; math.sqrt(2.0)` exercises the math module —
    requires `MICROPY_FLOAT_IMPL=DOUBLE` (so MicroPython's mp_float_t
    is a real double, lowered through uc386's x87 FPU path) plus
    libc-side implementations of the math functions modmath.c
    references at CORE_FEATURES (sin/cos/tan/asin/acos/atan/atan2/
    exp/log/pow/sqrt/floor/ceil/trunc/fmod/copysign/fabs/ldexp/
    nearbyint, plus modf for repr).

    Also pin the constant pi (gated on MICROPY_PY_MATH_CONSTANTS at
    EXTRA_FEATURES — we don't have it, so this test only checks
    sqrt). Result `1.414...` proves the FPU path works end-to-end."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import math\n"
                  b"print(math.sqrt(2.0))\n"
                  b"print(math.pi)\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "1.41421" in res.stdout, (
        f"expected sqrt(2) ≈ 1.41421..., got: {res.stdout!r}"
    )
    # math.pi static-init was producing 3.0 (`(mp_float_t)M_PI` cast
    # truncated through uc386's `_const_eval` Cast handler before
    # the integer-fold-refused-for-float fix).
    assert "3.14159" in res.stdout, (
        f"expected math.pi ≈ 3.14159..., got: {res.stdout!r}"
    )


def test_micropython_float_arithmetic(micropython_bin: Path) -> None:
    """`1.5 + 2.5` exercises the FPU path end-to-end inside the REPL:
    lex → parse FloatLiteral → BINARY_OP + → mp_float_t add → repr
    → print. With MICROPY_FLOAT_FORMAT_IMPL=EXACT and the
    verify-retry loop in py/formatfloat.c, the repr is
    CPython-compatible — `4.0` prints as `4.0`, not as
    `3.999999999999997`."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"print(1.5 + 2.5)\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "\n4.0\n" in res.stdout, (
        f"expected `4.0` (exact repr via verify-retry), got: "
        f"{res.stdout!r}"
    )


def test_micropython_float_repr_round_trips(micropython_bin: Path) -> None:
    """The EXACT formatter (MICROPY_FLOAT_FORMAT_IMPL_EXACT) picks
    the SHORTEST decimal that round-trips through the parser back
    to the same double. Pre-fix (APPROX), `print(1e10)` showed as
    `09999999999.99998` and `print(0.1)` as `0.0999999999999999`.
    Post-fix, simple values get their canonical literal form."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"print(0.1)\n"
                  b"print(1e10)\n"
                  b"print(0.1 + 0.2)\n"
                  b"print(1.0 / 3.0)\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "\n0.1\n" in res.stdout, (
        f"expected `0.1` exact, got: {res.stdout!r}"
    )
    assert "\n10000000000.0\n" in res.stdout, (
        f"expected `10000000000.0` exact for 1e10, got: "
        f"{res.stdout!r}"
    )
    # 0.1 + 0.2 has a real precision artifact in IEEE 754 doubles —
    # the actual sum is 0.30000000000000004, which CPython prints
    # similarly. Just check the stable 0.3000... prefix.
    assert "0.3000000000000001" in res.stdout or "0.30000000000000004" in res.stdout, (
        f"expected canonical 0.1+0.2 repr, got: {res.stdout!r}"
    )
    assert "0.3333333333333333" in res.stdout, (
        f"expected `0.333...` for 1/3, got: {res.stdout!r}"
    )


def test_micropython_math_special_functions(micropython_bin: Path) -> None:
    """`MICROPY_PY_MATH_SPECIAL_FUNCTIONS` opens hyperbolic +
    log2 + expm1 + erf/erfc surface. Pin a small subset
    (sinh/cosh ≈ identity at 0, log2(8) = 3, atanh(0.5) ≈ 0.549,
    erf(0.5) ≈ 0.520) so a future libc-side regression in any of
    the FPU primitives shows up here."""
    import math as cmath_local
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import math\n"
                  b"print(round(math.sinh(0.0), 4))\n"
                  b"print(round(math.cosh(0.0), 4))\n"
                  b"print(round(math.log2(8.0), 4))\n"
                  b"print(round(math.atanh(0.5), 4))\n"
                  b"print(round(math.erf(0.5), 4))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    # Each result is rounded to 4 decimal digits in-Python before
    # printing, dodging the formatter-precision quirk for sub-1
    # results (those don't need MAX_MANTISSA_DIGITS=19 anyway).
    expected = [
        ("sinh(0)", 0.0),
        ("cosh(0)", 1.0),
        ("log2(8)", 3.0),
        ("atanh(0.5)", round(cmath_local.atanh(0.5), 4)),
        ("erf(0.5)", round(cmath_local.erf(0.5), 4)),
    ]
    out_lines = [l for l in res.stdout.splitlines() if l and l[0].isdigit() or (l and l[0] == '-')]
    assert len(out_lines) >= 5, (
        f"expected 5 numeric lines, got: {res.stdout!r}"
    )
    for (label, exp), actual_str in zip(expected, out_lines):
        actual = float(actual_str)
        # Abramowitz erf is good to ~1.5e-7; round-to-4 leaves
        # plenty of room for that.
        assert abs(actual - exp) < 1e-3, (
            f"{label}: expected ≈ {exp}, got {actual}"
        )


def test_micropython_import_time_ticks(micropython_bin: Path) -> None:
    """`import time; time.ticks_ms()` exercises the BIOS-tick-backed
    HAL path: lib/i386_dos_libc.asm:_bios_ticks (INT 1Ah AH=00h)
    + uc386-dos/mphal_uc386dos.c's mp_hal_ticks_ms scaling.
    Pin `ticks_diff(after, before) >= 0` and that two consecutive
    reads don't move backwards — that's the contract user code
    relies on."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import time\n"
                  b"a = time.ticks_ms()\n"
                  b"b = time.ticks_ms()\n"
                  b"print(time.ticks_diff(b, a) >= 0)\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "True" in res.stdout, (
        f"expected ticks_diff >= 0, got: {res.stdout!r}"
    )


def test_micropython_time_sleep_ms(micropython_bin: Path) -> None:
    """`time.sleep_ms(60)` busy-waits one BIOS tick (~55 ms). Pin
    that ticks_ms advances by at least 50 ms across the call —
    proves mp_hal_delay_ms wires through to the BIOS counter and
    actually polls. Without the BIOS poll loop the call returns
    instantly and ticks_diff stays at 0."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import time\n"
                  b"a = time.ticks_ms()\n"
                  b"time.sleep_ms(60)\n"
                  b"b = time.ticks_ms()\n"
                  b"print(time.ticks_diff(b, a) >= 50)\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "True" in res.stdout, (
        f"expected ticks advance >= 50ms after sleep, got: "
        f"{res.stdout!r}"
    )


def test_micropython_time_time_dos_rtc(micropython_bin: Path) -> None:
    """`time.time()` reads the DOS RTC via INT 21h AH=0x2A (date)
    + AH=0x2C (time-of-day) and converts to seconds-since-epoch
    via upstream's timeutils. dos_emu synthesizes
    2026-05-03 12:34:00 (see src/uc386/dos_emu.py's INT 21h
    handlers). With MICROPY_TIMESTAMP_IMPL=UINT and
    MICROPY_EPOCH_IS_2000=1 (default), the expected value is
    seconds_since_2000(2026, 5, 3, 12, 34, 0) ≈ 8.31e8 — fits a
    32-bit small int. Pin a wide ballpark (epoch in [year 2024,
    year 2030]) so the test isn't fragile if the synthetic date
    moves forward."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import time\n"
                  b"t = time.time()\n"
                  b"print('OK', 757382400 < t < 946684800)\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "OK True" in res.stdout, (
        f"expected time.time() in [2024..2030] window, got: "
        f"{res.stdout!r}"
    )


def test_micropython_time_localtime_dos_rtc(micropython_bin: Path) -> None:
    """`time.localtime()` (no arg) reads the DOS RTC and returns
    a struct_time. Pin the year/month/day fields against the
    synthetic 2026-05-03 12:34:00 from dos_emu — proves the
    INT 21h date+time → struct_time conversion path is
    end-to-end correct."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import time\n"
                  b"t = time.localtime()\n"
                  b"print(t[0], t[1], t[2], t[3], t[4], t[5])\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "2026 5 3 12 34 0" in res.stdout, (
        f"expected localtime() == 2026-05-03 12:34:00, got: "
        f"{res.stdout!r}"
    )


def test_micropython_time_gmtime_constant(micropython_bin: Path) -> None:
    """`time.gmtime(0)` (epoch start) returns a deterministic
    struct_time independent of the RTC. With
    MICROPY_EPOCH_IS_2000=1 (default), epoch 0 is
    2000-01-01 00:00:00 UTC (Saturday, day 1)."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import time\n"
                  b"print(time.gmtime(0))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "(2000, 1, 1, 0, 0, 0, 5, 1)" in res.stdout, (
        f"expected gmtime(0) == 2000-01-01 epoch tuple, got: "
        f"{res.stdout!r}"
    )


def test_micropython_time_time_ns_dos_rtc(micropython_bin: Path) -> None:
    """`time.time_ns()` = time.time() * 1e9 with second resolution
    (DOS RTC has no sub-second contribution). Checks that the
    nanosecond value is the second value × 1e9. Validates the
    `MICROPY_LONGINT_IMPL_LONGLONG` int support — without it,
    `mp_obj_new_int_from_ull` is a stub that always raises
    `OverflowError`."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import time\n"
                  b"s = time.time()\n"
                  b"ns = time.time_ns()\n"
                  b"print('OK' if ns == s * 1000000000 else 'BAD', ns)\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "OK" in res.stdout, (
        f"expected time_ns == time * 1e9, got: {res.stdout!r}"
    )


def test_micropython_long_long_int_arithmetic(micropython_bin: Path) -> None:
    """`MICROPY_LONGINT_IMPL_LONGLONG` enables heap-allocated
    `mp_obj_int_t` for values that don't fit a small int. Checks
    `2**62` (= 4611686018427387904) and its negation parse and
    print correctly — both well outside the 31-bit small-int
    range. Without long-long support, these raise OverflowError
    via `mp_obj_new_int_from_ll`."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"x = 2**62\n"
                  b"print(x)\n"
                  b"print(-x)\n"
                  b"print(x + 1 == 4611686018427387905)\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "4611686018427387904" in res.stdout, (
        f"expected 2**62 = 4611686018427387904, got: {res.stdout!r}"
    )
    assert "-4611686018427387904" in res.stdout, (
        f"expected -(2**62) = -4611686018427387904, got: {res.stdout!r}"
    )
    assert "True" in res.stdout, (
        f"expected x + 1 = 4611686018427387905, got: {res.stdout!r}"
    )


def test_micropython_sys_exit_raises_systemexit(micropython_bin: Path) -> None:
    """`sys.exit(N)` raises `SystemExit(N)`. With
    MICROPY_PY_SYS_EXIT=1, the function is exposed in sys's
    globals table; `mp_sys_exit` (modsys.c:194) calls
    `mp_raise_type_arg(&mp_type_SystemExit, args[0])`. We catch
    here so the harness can assert the exit code without
    pyexec_friendly_repl bailing the whole REPL."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import sys\n"
                  b"try:\n"
                  b"    sys.exit(42)\n"
                  b"except SystemExit as e:\n"
                  b"    print('caught', e)\n"
                  b"\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "caught 42" in res.stdout, (
        f"expected `caught 42` after sys.exit(42), got: "
        f"{res.stdout!r}"
    )


def test_micropython_sys_modules_is_dict(micropython_bin: Path) -> None:
    """`sys.modules` is a dict initialized by `mp_init` via
    `mp_obj_dict_init(&MP_STATE_VM(mp_loaded_modules_dict), N)`.
    With MICROPY_PY_SYS_MODULES=1 it's exposed as `sys.modules`.

    Built-in modules (gc, sys, etc.) don't auto-register —
    `builtinimport.c` only stores user-imported and `__main__`
    entries (mp_obj_dict_store call at line 483). So we verify
    `sys.modules` is a real dict by storing something into it
    and reading it back."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import sys\n"
                  b"print(type(sys.modules).__name__)\n"
                  b"sys.modules['probe'] = 42\n"
                  b"print('probe' in sys.modules, sys.modules['probe'])\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "\ndict\n" in res.stdout, (
        f"expected sys.modules type to be dict, got: {res.stdout!r}"
    )
    assert "True 42" in res.stdout, (
        f"expected stored entry to round-trip, got: {res.stdout!r}"
    )


def test_micropython_sys_path_is_mutable_list(micropython_bin: Path) -> None:
    """`sys.path` (with MICROPY_PY_SYS_PATH=1) is exposed via
    attribute delegation: modsys.c registers
    `mp_module_sys_attr` as the sys-module attr handler, which
    dispatches `sys.path` reads/writes against
    `MP_STATE_VM(sys_mutable[MP_SYS_MUTABLE_PATH])`. mp_init's
    `MICROPY_PY_SYS_PATH_ARGV_DEFAULTS` block initializes the
    list with one entry — empty string = current directory.

    The hand-rolled `build/genhdr/moduledefs.h` (template in
    build.sh) gains a `MICROPY_MODULE_DELEGATIONS` macro that
    routes `mp_module_sys` → `mp_module_sys_attr` for objmodule.c's
    delegation table; without that, every sys.* attr access raises
    AttributeError."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import sys\n"
                  b"print(sys.path)\n"
                  b"sys.path.append('foo')\n"
                  b"print(sys.path)\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    # Initial: ['']
    assert "\n['']\n" in res.stdout, (
        f"expected initial sys.path == [''], got: {res.stdout!r}"
    )
    # After append: ['', 'foo']
    assert "['', 'foo']" in res.stdout, (
        f"expected sys.path == ['', 'foo'] after append, got: "
        f"{res.stdout!r}"
    )


def test_micropython_sys_argv_is_empty_list(micropython_bin: Path) -> None:
    """`sys.argv` is initialized by main.c via
    `mp_obj_list_init(&MP_STATE_VM(mp_sys_argv_obj), 0)` (sed-
    patched into upstream/ports/minimal/main.c by build.sh
    after mp_init). The minimal port doesn't tokenize the DOS
    PSP cmdline, so argv is an empty list — matching what
    `python -c '...'` sees. Verify the type is `list` and
    `len(argv) == 0`, plus `argv.append('foo')` works (proving
    the list object is real, not a stub)."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import sys\n"
                  b"print(type(sys.argv).__name__, len(sys.argv))\n"
                  b"sys.argv.append('foo')\n"
                  b"print(sys.argv)\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "list 0" in res.stdout, (
        f"expected `list 0` (type=list, len=0), got: {res.stdout!r}"
    )
    assert "['foo']" in res.stdout, (
        f"expected `['foo']` after append, got: {res.stdout!r}"
    )


def test_micropython_builtin_help(micropython_bin: Path) -> None:
    """`help()` (no arg) prints the default help text from
    `py/builtinhelp.c:mp_help_default_text`. Pin the
    `Welcome to MicroPython!` first line as a smoke."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"help()\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "Welcome to MicroPython!" in res.stdout, (
        f"expected default help banner, got: {res.stdout!r}"
    )


def test_micropython_module_dunder_file(micropython_bin: Path) -> None:
    """`mymod.__file__` is set to the source path the lexer was
    given. With MICROPY_MODULE___FILE__=1, builtinimport.c's
    `do_load_from_lexer` (line 158) calls `mp_store_attr` on the
    module with `__file__` = `lex->source_name`. Our
    `uc386-dos/file_uc386dos.c:mp_lexer_new_from_file` builds the
    lexer from the import filename qstr."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import myhelper\n"
                  b"print(myhelper.__file__)\n"
                  b"print(myhelper.value)\n"
                  b"\x04"
              ),
              vfiles_init={b"myhelper.py": b"value = 42\n"},
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "myhelper.py" in res.stdout, (
        f"expected myhelper.__file__ == 'myhelper.py', got: "
        f"{res.stdout!r}"
    )
    assert "\n42\n" in res.stdout, (
        f"expected myhelper.value == 42, got: {res.stdout!r}"
    )


def test_micropython_stack_check_catches_runaway_recursion(micropython_bin: Path) -> None:
    """`MICROPY_STACK_CHECK=1` enables `mp_stack_check()` calls
    in the VM dispatcher. Combined with the `mp_stack_ctrl_init`
    + `mp_stack_set_limit(0xC0000)` calls main.c does at startup
    (sed-patched in by build.sh), unbounded recursion raises
    `RuntimeError("maximum recursion depth exceeded")` instead
    of running off the end of the dos_emu 1 MB stack and
    triggering UC_ERR_WRITE_UNMAPPED."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"def f(n):\n"
                  b"  return f(n+1)\n"
                  b"\n"
                  b"try:\n"
                  b"  f(0)\n"
                  b"except RuntimeError as e:\n"
                  b"  print('caught:', e)\n"
                  b"\n"
                  b"\x04"
              ),
              timeout_seconds=30.0,
              instruction_limit=8_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "maximum recursion depth exceeded" in res.stdout, (
        f"expected RuntimeError message about recursion depth, "
        f"got: {res.stdout!r}"
    )


def test_micropython_random_seeded(micropython_bin: Path) -> None:
    """`random.seed(N)` then `random.getrandbits(8)` is
    deterministic — yasmarang PRNG. Pin a value against the
    seed-42 sequence."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import random\n"
                  b"random.seed(42)\n"
                  b"print(random.getrandbits(8))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "\n156\n" in res.stdout, (
        f"expected `156` (yasmarang seed=42 first 8-bit getrandbits), "
        f"got: {res.stdout!r}"
    )


def test_micropython_binascii_hexlify(micropython_bin: Path) -> None:
    """`binascii.hexlify(b'abc')` → `b'616263'`. Pin a basic
    round-trip through the binascii module's hex codec."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import binascii\n"
                  b"print(binascii.hexlify(b'abc'))\n"
                  b"print(binascii.unhexlify(b'616263'))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "b'616263'" in res.stdout, (
        f"expected hexlify result, got: {res.stdout!r}"
    )
    assert "b'abc'" in res.stdout, (
        f"expected unhexlify result, got: {res.stdout!r}"
    )


def test_micropython_hashlib_sha256(micropython_bin: Path) -> None:
    """`hashlib.sha256(b'hello').hexdigest()` → known SHA-256.
    Validates the inline-included `lib/crypto-algorithms/sha256.c`
    impl produces the canonical RFC-6234 reference output."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import hashlib\n"
                  b"import binascii\n"
                  b"h = hashlib.sha256()\n"
                  b"h.update(b'hello')\n"
                  b"print(binascii.hexlify(h.digest()))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824" in res.stdout, (
        f"expected SHA-256(b'hello') canonical output, got: "
        f"{res.stdout!r}"
    )


def test_micropython_os_mkdir_chdir_getcwd(micropython_bin: Path) -> None:
    """`os.mkdir` + `os.chdir` + `os.getcwd` round-trip backed by
    INT 21h AH=0x39/0x3B/0x47 via uc386's libc.

    dos_emu maintains a synthetic vdirs set + vcwd string that the
    handlers read/write."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import os\n"
                  b"print(os.getcwd())\n"
                  b"os.mkdir('newdir')\n"
                  b"os.chdir('newdir')\n"
                  b"print(os.getcwd())\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "C:\\\n" in res.stdout, (
        f"expected initial cwd 'C:\\', got: {res.stdout!r}"
    )
    assert "C:\\newdir" in res.stdout, (
        f"expected cwd 'C:\\newdir' after chdir, got: {res.stdout!r}"
    )


def test_micropython_deflate_inflate_roundtrip(micropython_bin: Path) -> None:
    """`deflate.DeflateIO(buf, deflate.RAW)` reads from a buffer
    of raw deflate bytes (no zlib header) and inflates them.
    Validates upstream's tinflate.c works through uc386's
    compiler. The hex blob is `zlib.compress(b'hello world')[2:-4]`
    from CPython — strips the 2-byte zlib header and 4-byte
    adler32 trailer."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import deflate\n"
                  b"import io\n"
                  b"import binascii\n"
                  b"raw = binascii.unhexlify(b'cb48cdc9c95728cf2fca490100')\n"
                  b"buf = io.BytesIO(raw)\n"
                  b"d = deflate.DeflateIO(buf, deflate.RAW)\n"
                  b"print(d.read())\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "b'hello world'" in res.stdout, (
        f"expected `b'hello world'` from inflate, got: {res.stdout!r}"
    )


def test_micropython_binascii_crc32(micropython_bin: Path) -> None:
    """`binascii.crc32(b'hello world')` returns the canonical
    CRC32 = 0x0d4a1185 = 222957957. Backed by uzlib's crc32.c
    via moddeflate.c's inline include (the symbol is non-static
    and binascii.c references it as an extern)."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import binascii\n"
                  b"print(binascii.crc32(b'hello world'))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "222957957" in res.stdout, (
        f"expected canonical CRC32 of 'hello world', got: "
        f"{res.stdout!r}"
    )


def test_micropython_heapq(micropython_bin: Path) -> None:
    """`heapq.heappush` + `heappop` round-trip — pop returns
    elements in sorted order."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import heapq\n"
                  b"h = []\n"
                  b"for x in [3, 1, 2, 5, 4]:\n"
                  b"    heapq.heappush(h, x)\n"
                  b"\n"
                  b"print([heapq.heappop(h) for _ in range(5)])\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "[1, 2, 3, 4, 5]" in res.stdout, (
        f"expected sorted heap-pop, got: {res.stdout!r}"
    )


def test_micropython_os_stat(micropython_bin: Path) -> None:
    """`os.stat(path)` — returns 10-tuple matching CPython's
    `os.stat_result`. Index 6 is `st_size`. Backed by uc386's
    libc `stat()` (INT 21h-based)."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import os\n"
                  b"st = os.stat('hi.txt')\n"
                  b"print(len(st), st[6])\n"
                  b"\x04"
              ),
              vfiles_init={b"hi.txt": b"hello world"},
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "10 11" in res.stdout, (
        f"expected stat result tuple of len 10 with st_size=11, "
        f"got: {res.stdout!r}"
    )


def test_micropython_os_listdir(micropython_bin: Path) -> None:
    """`os.listdir()` enumerates files via DOS find-first
    (INT 21h AH=0x4E) + find-next (AH=0x4F) using a static DTA
    in lib/i386_dos_libc.asm. dos_emu's handlers iterate the
    in-memory `vfiles` and `vdirs` and write filenames into the
    caller's DTA at +30."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import os\n"
                  b"print(sorted(os.listdir()))\n"
                  b"\x04"
              ),
              vfiles_init={
                  b"a.txt": b"A",
                  b"b.txt": b"B",
                  b"config.ini": b"",
              },
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "['a.txt', 'b.txt', 'config.ini']" in res.stdout, (
        f"expected sorted listing of seeded vfiles, got: "
        f"{res.stdout!r}"
    )


def test_micropython_os_rename_unlink(micropython_bin: Path) -> None:
    """`os.rename` (INT 21h AH=0x56) + `os.unlink` (AH=0x41).
    Create a file, rename it, read the new name, unlink, verify
    gone."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import os\n"
                  b"with open('a.txt', 'w') as f:\n"
                  b"    f.write('hello')\n"
                  b"\n"
                  b"os.rename('a.txt', 'b.txt')\n"
                  b"print(open('b.txt').read())\n"
                  b"os.unlink('b.txt')\n"
                  b"try:\n"
                  b"    open('b.txt')\n"
                  b"except OSError:\n"
                  b"    print('gone')\n"
                  b"\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "hello" in res.stdout, (
        f"expected `hello` after rename round-trip, got: {res.stdout!r}"
    )
    assert "gone" in res.stdout, (
        f"expected `gone` after unlink, got: {res.stdout!r}"
    )


def test_micropython_math_gamma(micropython_bin: Path) -> None:
    """`math.gamma` and `math.lgamma`. Pre-fix these were NaN
    stubs in lib/i386_dos_libc.asm. Post-fix they're a Lanczos
    approximation in C (uc386-dos/math_gamma.c).

    gamma(N) = (N-1)! → gamma(5) ≈ 24, gamma(1) = 1.
    gamma(0.5) = √π ≈ 1.7724538509.
    lgamma(10) = log(9!) ≈ log(362880) ≈ 12.8018."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import math\n"
                  b"print(int(math.gamma(5) + 0.5))\n"
                  b"print(int(math.gamma(0.5) * 10000))\n"
                  b"print(int(math.lgamma(10) * 1000))\n"
                  b"print(math.gamma(1))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "\n24\n" in res.stdout, (
        f"expected gamma(5) ≈ 24 (4!), got: {res.stdout!r}"
    )
    # sqrt(pi) = 1.7724538509...; the Lanczos approx ~10⁻¹⁰ accuracy
    # gives 17724 or 17725 after *10000+truncation.
    assert "\n17724\n" in res.stdout or "\n17725\n" in res.stdout, (
        f"expected gamma(0.5) ≈ √π → 1.7724/1.7725, got: {res.stdout!r}"
    )
    # log(9!) = 12.801827...
    assert "\n12801\n" in res.stdout, (
        f"expected lgamma(10) ≈ 12.801, got: {res.stdout!r}"
    )
    assert "\n1.0\n" in res.stdout, (
        f"expected gamma(1) == 1.0, got: {res.stdout!r}"
    )


def test_micropython_cmath_basic(micropython_bin: Path) -> None:
    """`import cmath` complex-number math. Validates abs(),
    cmath.exp on imaginary axis (Euler's formula:
    exp(i*x) = cos(x) + i*sin(x))."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import cmath\n"
                  b"print(abs(complex(3, 4)))\n"
                  b"r = cmath.exp(1j)\n"
                  # int truncation avoids round()'s float-precision quirk;
                  # check the high-bit digits land in the right place.
                  b"print(int(r.real * 10000), int(r.imag * 10000))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "5.0" in res.stdout, (
        f"expected abs(3+4j) == 5.0, got: {res.stdout!r}"
    )
    # exp(i) = cos(1) + i*sin(1) ≈ 0.5403 + 0.8415i
    assert "5402 8414" in res.stdout or "5403 8414" in res.stdout, (
        f"expected Euler's formula int*10000 ≈ (5403, 8414), got: "
        f"{res.stdout!r}"
    )


def test_micropython_re_match_groups(micropython_bin: Path) -> None:
    """`re.match(pattern, string)` with capture groups. Validates
    the `lib/re1.5/*.c` regex engine end-to-end through the re
    module's MicroPython surface (`m.group(N)` indexing)."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import re\n"
                  b"m = re.match(r'(\\w+)\\s+(\\w+)', 'hello world')\n"
                  b"print(m.group(1), m.group(2))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "hello world" in res.stdout, (
        f"expected captured groups `hello world`, got: {res.stdout!r}"
    )


def test_micropython_uctypes_struct_roundtrip(micropython_bin: Path) -> None:
    """`uctypes` (binary struct access for buffer-like objects).
    Defines a layout dict, pins it onto a bytearray, writes
    fields by name, verifies the underlying bytes are correct
    little-endian. With MICROPY_PY_UCTYPES=1 build_port.sh adds
    extmod/moductypes.c to the source list and build.sh's
    moduledefs.h registers `uctypes`."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import uctypes\n"
                  b"buf = bytearray(8)\n"
                  b"desc = {'a': 0 | uctypes.UINT32, 'b': 4 | uctypes.UINT32}\n"
                  b"s = uctypes.struct(uctypes.addressof(buf), desc, "
                  b"uctypes.LITTLE_ENDIAN)\n"
                  b"s.a = 0xDEADBEEF\n"
                  b"s.b = 0xCAFEBABE\n"
                  b"print(buf.hex())\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    # Little-endian: 0xDEADBEEF → ef be ad de, 0xCAFEBABE → be ba fe ca
    assert "efbeaddebebafeca" in res.stdout, (
        f"expected little-endian 0xDEADBEEF + 0xCAFEBABE bytes "
        f"(`efbeaddebebafeca`), got: {res.stdout!r}"
    )


def test_micropython_extra_features_compile(micropython_bin: Path) -> None:
    """`compile()` is gated at EXTRA_FEATURES (already on at CORE
    via `MICROPY_PY_BUILTINS_COMPILE && MICROPY_ENABLE_COMPILER`).
    Compiles a small string-source program and exec's it, proving
    the runtime parser + bytecode emitter are both reachable from
    user code."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"c = compile('print(2+3)', '<s>', 'exec')\n"
                  b"exec(c)\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "\n5\n" in res.stdout, (
        f"expected compile+exec → 5, got: {res.stdout!r}"
    )


def test_micropython_extra_features_memoryview(micropython_bin: Path) -> None:
    """`memoryview` is EXTRA_FEATURES-gated. Pin construction over
    a bytearray + indexing returning the right byte value."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"mv = memoryview(bytearray(b'abc'))\n"
                  b"print(mv[1])\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    # 'b' = 0x62 = 98 decimal.
    assert "\n98\n" in res.stdout, (
        f"expected memoryview[1] = 98 (=ord('b')), got: {res.stdout!r}"
    )


def test_micropython_extra_features_collections_deque(micropython_bin: Path) -> None:
    """`collections.deque` is EXTRA_FEATURES-gated. Pin a basic
    append + popleft round-trip — proves objdeque.c's static-init
    + the FIFO behavior end-to-end."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"from collections import deque\n"
                  b"d = deque((), 4)\n"
                  b"d.append(1)\n"
                  b"d.append(2)\n"
                  b"print(d.popleft())\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "\n1\n" in res.stdout, (
        f"expected deque popleft → 1, got: {res.stdout!r}"
    )


def test_micropython_extra_features_math_constants(micropython_bin: Path) -> None:
    """`math.tau` / `math.inf` / `math.nan` are EXTRA_FEATURES-gated
    constants. Pin tau = 2*pi (≈6.28) and inf-arithmetic (`inf > 1e300`)
    so a future config change doesn't silently drop them. The nan
    check uses `math.isnan` (also EXTRA-gated indirectly via
    `MICROPY_PY_MATH_ISCLOSE`-shape API surface)."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import math\n"
                  b"print(round(math.tau, 4))\n"
                  b"print(math.inf > 1e300)\n"
                  b"print(math.isnan(math.nan))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    # `round(math.tau, 4)` → 6.2832 in math, but stored as the
    # nearest IEEE-754 double, which prints as `6.28319999...`
    # under uc386's APPROX float formatter. Match the prefix.
    assert "6.2831" in res.stdout or "6.2832" in res.stdout, (
        f"expected math.tau ≈ 6.2832, got: {res.stdout!r}"
    )
    # `inf > 1e300` and `isnan(nan)` both → True.
    assert res.stdout.count("True") >= 2, (
        f"expected 2 True (inf comparison + isnan), got: {res.stdout!r}"
    )


def test_micropython_extra_features_math_factorial(micropython_bin: Path) -> None:
    """`math.factorial` is EXTRA_FEATURES-gated. Pin the standard
    `5! = 120` value end-to-end."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"import math\nprint(math.factorial(5))\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "\n120\n" in res.stdout, (
        f"expected math.factorial(5) → 120, got: {res.stdout!r}"
    )


def test_micropython_extra_features_math_isclose(micropython_bin: Path) -> None:
    """`math.isclose` is EXTRA_FEATURES-gated. Pin both the
    matching and non-matching cases (1.0 vs 1.0+1e-10 are close;
    1.0 vs 1.5 are not)."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import math\n"
                  b"print(math.isclose(1.0, 1.0 + 1e-10))\n"
                  b"print(math.isclose(1.0, 1.5))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    # First True, second False.
    assert "True" in res.stdout and "False" in res.stdout, (
        f"expected True + False from math.isclose, got: {res.stdout!r}"
    )


def test_micropython_extra_features_bytes_hex(micropython_bin: Path) -> None:
    """`bytes.hex` / `bytes.fromhex` are EXTRA-gated. Pin both
    directions of the round-trip."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"print(b'\\x12\\x34'.hex())\n"
                  b"print(bytes.fromhex('5678'))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "1234" in res.stdout, (
        f"expected b'\\x12\\x34'.hex() → '1234', got: {res.stdout!r}"
    )
    assert "Vx" in res.stdout, (
        f"expected fromhex('5678') → b'Vx' (0x56='V', 0x78='x'), "
        f"got: {res.stdout!r}"
    )


def test_micropython_extra_features_fstring(micropython_bin: Path) -> None:
    """f-strings are EXTRA-gated via `MICROPY_PY_FSTRINGS`. Pin
    both literal substitution and the formatter."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"x = 42\n"
                  b"print(f'val={x}')\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "val=42" in res.stdout, (
        f"expected `val=42` from f-string, got: {res.stdout!r}"
    )


def test_micropython_extra_features_inplace_special(micropython_bin: Path) -> None:
    """`MICROPY_PY_ALL_INPLACE_SPECIAL_METHODS` lights up
    `__iadd__` etc. on class instances. Pin a small `__iadd__`
    override + `+=` round-trip."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"class V:\n"
                  b"    def __init__(s,x): s.x=x\n"
                  b"    def __iadd__(s,o): s.x+=o.x; return s\n"
                  b"\n"
                  b"v=V(2)\n"
                  b"v+=V(3)\n"
                  b"print(v.x)\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "\n5\n" in res.stdout, (
        f"expected V(2)+=V(3) → 5 via __iadd__, got: {res.stdout!r}"
    )


def test_micropython_extra_features_frozenset(micropython_bin: Path) -> None:
    """`frozenset` is gated on `MICROPY_PY_BUILTINS_FROZENSET`."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=b"print(frozenset([1,2,3]))\n\x04",
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "frozenset" in res.stdout, (
        f"expected frozenset repr, got: {res.stdout!r}"
    )


def test_micropython_extra_features_special_methods(micropython_bin: Path) -> None:
    """`MICROPY_PY_ALL_SPECIAL_METHODS` lights up class instance
    binary-op overrides (`__and__`, `__add__`, etc). Pin a small
    `__add__` override to prove the dispatch path resolves."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"class V:\n"
                  b"    def __init__(s, x): s.x = x\n"
                  b"    def __add__(s, o): return V(s.x + o.x).x\n"
                  b"\n"
                  b"print(V(2) + V(3))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "\n5\n" in res.stdout, (
        f"expected V(2)+V(3) → 5 via __add__, got: {res.stdout!r}"
    )


def test_micropython_open_read(micropython_bin: Path) -> None:
    """`open(path).read()` exercises the port-supplied
    `mp_builtin_open_obj` (uc386-dos/file_uc386dos.c) which wraps
    a DOS file handle. Backing INT 21h calls (open/read/close)
    flow through dos_emu's vfile layer for the test, but production
    just hits real DOS via `_open` etc. in lib/i386_dos_libc.asm."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"f = open('hello.txt')\n"
                  b"print(f.read())\n"
                  b"f.close()\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000,
              vfiles_init={b"hello.txt": b"Hello from disk!"})
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "Hello from disk!" in res.stdout, (
        f"expected file contents in stdout, got: {res.stdout!r}"
    )


def test_micropython_open_write_read_roundtrip(micropython_bin: Path) -> None:
    """`open(path, 'w').write(...)` + reopen + `.read()` exercises the
    full write path via INT 21h AH=0x40 (and create-via-AH=0x3D mode 1).
    Pin both that the write returns the byte count and that the data
    survives a close/reopen."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"f = open('out.txt', 'w')\n"
                  b"print(f.write('hello'))\n"
                  b"f.close()\n"
                  b"f = open('out.txt')\n"
                  b"print(f.read())\n"
                  b"f.close()\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000,
              vfiles_init={b"out.txt": b""})
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "\n5\n" in res.stdout, (
        f"expected write to return 5, got: {res.stdout!r}"
    )
    assert "\nhello\n" in res.stdout, (
        f"expected `hello` round-trip, got: {res.stdout!r}"
    )


def test_micropython_import_from_disk(micropython_bin: Path) -> None:
    """`import xxx` loads `xxx.py` from disk via the port-supplied
    `mp_lexer_new_from_file()` + `mp_import_stat()` pair. Pins the
    full lex → parse → compile → execute path on a non-trivial
    module body. The `_stat` impl in libc had to be fixed to
    zero-extend the DX:AX 32-bit-pos return from INT 21h AH=0x42
    SEEK_END (was inheriting stale upper-16 bits of EAX, leading
    to multi-MB phantom file sizes and `MemoryError: allocating
    17760257 bytes`)."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import mymod\n"
                  b"print(mymod.x, mymod.greet('world'))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000,
              vfiles_init={
                  b"mymod.py": (
                      b"x = 42\n"
                      b"def greet(name):\n"
                      b"    return 'hello, ' + name\n"
                  ),
              })
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "42 hello, world" in res.stdout, (
        f"expected `42 hello, world` from imported module, got: "
        f"{res.stdout!r}"
    )


def test_micropython_import_array(micropython_bin: Path) -> None:
    """`import array; array.array('i', ...)` — gated on
    `MICROPY_PY_ARRAY` (CORE_FEATURES). Pins typed-array storage
    + iteration."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import array\n"
                  b"a = array.array('i', [1,2,3])\n"
                  b"print(sum(a))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "\n6\n" in res.stdout, (
        f"expected sum 6 from int array, got: {res.stdout!r}"
    )


def test_micropython_os_getenv(micropython_bin: Path) -> None:
    """`os.getenv(name, default)` walks the DOS env block via
    libc's `_getenv` (INT 21h AH=0x62 → PSP[0x2C] → linear addr).
    dos_emu populates a fake env block from the `env=` kwarg."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import os\n"
                  b"print(os.getenv('PATH'))\n"
                  b"print(os.getenv('MISSING'))\n"
                  b"print(os.getenv('MISSING', 'fallback'))\n"
                  b"\x04"
              ),
              env={"PATH": "C:\\DOS;C:\\TOOLS", "USER": "wohl"},
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "C:\\DOS;C:\\TOOLS" in res.stdout, (
        f"expected PATH value, got: {res.stdout!r}"
    )
    assert "\nNone\n" in res.stdout, (
        f"expected None for missing key (no default), got: {res.stdout!r}"
    )
    assert "fallback" in res.stdout, (
        f"expected fallback default for missing key, got: {res.stdout!r}"
    )


def test_micropython_os_environ(micropython_bin: Path) -> None:
    """`os.environ()` snapshots the env block as a dict. Differs
    from CPython where `os.environ` is a live dict — MicroPython
    modules don't support attribute-getter delegation, so we expose
    a function instead. Snapshot is built by walking
    `dos_env_iter(0..)` and splitting on '='."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import os\n"
                  b"e = os.environ()\n"
                  b"print(sorted(e.items()))\n"
                  b"\x04"
              ),
              env={"FOO": "bar", "BAZ": "qux"},
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "('BAZ', 'qux')" in res.stdout and "('FOO', 'bar')" in res.stdout, (
        f"expected env tuples in dict, got: {res.stdout!r}"
    )


def test_micropython_base64(micropython_bin: Path) -> None:
    """`base64.b64encode` / `b64decode` / `b16encode` / `b16decode`
    — port-supplied module (uc386-dos/base64_uc386dos.c) implementing
    RFC 4648 base64 + base16 directly. Pin RFC 4648 test vectors so
    encoder/decoder don't drift."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import base64\n"
                  b"print(base64.b64encode(b'foobar'))\n"  # RFC 4648
                  b"print(base64.b64decode(b'Zm9vYmFy'))\n"
                  b"print(base64.b64encode(b''))\n"
                  b"print(base64.b64encode(b'f'))\n"
                  b"print(base64.b64encode(b'fo'))\n"
                  b"print(base64.b16encode(b'\\xde\\xad'))\n"
                  b"print(base64.b16decode(b'DEADBEEF'))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    # RFC 4648 §10 vectors.
    assert "b'Zm9vYmFy'" in res.stdout, f"foobar→: {res.stdout!r}"
    assert "b'foobar'" in res.stdout, f"decode foobar: {res.stdout!r}"
    assert "b''" in res.stdout, f"empty: {res.stdout!r}"
    assert "b'Zg=='" in res.stdout, f"f→Zg==: {res.stdout!r}"
    assert "b'Zm8='" in res.stdout, f"fo→Zm8=: {res.stdout!r}"
    assert "b'DEAD'" in res.stdout, f"b16 dead: {res.stdout!r}"
    assert "b'\\xde\\xad\\xbe\\xef'" in res.stdout, (
        f"b16 decode: {res.stdout!r}"
    )


def test_micropython_shutil_copy_move(micropython_bin: Path) -> None:
    """`shutil.copy` reads src → writes dst (open/read/write loop
    in 4 KB chunks). `shutil.move` tries os.rename first (atomic
    same-volume) and falls back to copy+unlink."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import shutil\n"
                  b"shutil.copy('src.txt', 'dst.txt')\n"
                  b"print(open('dst.txt').read())\n"
                  b"shutil.move('dst.txt', 'moved.txt')\n"
                  b"print(open('moved.txt').read())\n"
                  b"try:\n"
                  b"    open('dst.txt')\n"
                  b"except OSError:\n"
                  b"    print('original gone')\n"
                  b"\n"
                  b"\x04"
              ),
              vfiles_init={b"src.txt": b"copied bytes"},
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    # copy + read-back yields the source bytes.
    assert "copied bytes" in res.stdout, f"copy content: {res.stdout!r}"
    # move + read-back via the new name still works.
    assert res.stdout.count("copied bytes") >= 2, (
        f"move read-back: {res.stdout!r}"
    )
    # Original name is gone after rename.
    assert "original gone" in res.stdout, (
        f"move removes src: {res.stdout!r}"
    )


def test_micropython_tempfile(micropython_bin: Path) -> None:
    """`tempfile.gettempdir()` reads TEMP/TMP env vars or falls
    back to "C:\\". `tempfile.mktemp()` returns a path that doesn't
    exist (probed via libc stat), with a counter that increments
    across calls so consecutive mktemp results never collide."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import tempfile\n"
                  b"print('td=', tempfile.gettempdir())\n"
                  b"a = tempfile.mktemp(suffix='.txt')\n"
                  b"b = tempfile.mktemp(suffix='.txt')\n"
                  b"print('a=', a)\n"
                  b"print('b=', b)\n"
                  b"print('distinct=', a != b)\n"
                  b"\x04"
              ),
              env={"TEMP": "C:\\TEMP"},
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "td= C:\\TEMP" in res.stdout, f"gettempdir env: {res.stdout!r}"
    assert "C:\\TEMP\\tmp" in res.stdout, (
        f"mktemp uses tempdir: {res.stdout!r}"
    )
    assert ".txt" in res.stdout, f"mktemp uses suffix: {res.stdout!r}"
    assert "distinct= True" in res.stdout, (
        f"consecutive mktemp differ: {res.stdout!r}"
    )


def test_micropython_os_path_join_split(micropython_bin: Path) -> None:
    """`os.path.join` / `split` / `splitext` / `basename` / `dirname`
    — port-supplied submodule (uc386-dos/path_uc386dos.c). Backslash
    is the canonical separator; both '\\' and '/' are accepted on
    input. Drive prefixes (`C:`) reset join() like CPython on Win.
    Test pins the major edge cases."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import os\n"
                  b"print(os.path.join('a', 'b', 'c'))\n"
                  b"print(os.path.join('C:\\\\dos', 'foo.txt'))\n"
                  b"print(os.path.join('a', 'C:\\\\absolute'))\n"
                  b"print(os.path.split('C:\\\\dir\\\\file.txt'))\n"
                  b"print(os.path.splitext('archive.tar.gz'))\n"
                  b"print(os.path.basename('C:\\\\dos\\\\app.exe'))\n"
                  b"print(os.path.dirname('C:\\\\dos\\\\app.exe'))\n"
                  b"print(os.path.sep)\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    # join() with backslash separator.
    assert "a\\b\\c" in res.stdout, f"join 3 parts: {res.stdout!r}"
    assert "C:\\dos\\foo.txt" in res.stdout, f"join drive: {res.stdout!r}"
    # Drive-letter prefix in arg 2 resets — matches CPython on Win.
    assert "C:\\absolute" in res.stdout, f"join abs reset: {res.stdout!r}"
    # split() returns (head, tail).
    assert "('C:\\\\dir', 'file.txt')" in res.stdout, (
        f"split tuple: {res.stdout!r}"
    )
    # splitext() — only last dot, ext includes the dot.
    assert "('archive.tar', '.gz')" in res.stdout, (
        f"splitext: {res.stdout!r}"
    )
    assert "app.exe" in res.stdout, f"basename: {res.stdout!r}"
    assert "C:\\dos" in res.stdout, f"dirname: {res.stdout!r}"


def test_micropython_os_path_exists(micropython_bin: Path) -> None:
    """`os.path.exists` / `isfile` against a seeded vfile. Backed
    by libc's stat() (INT 21h AH=0x42 lseek-to-end). True for a
    file we explicitly seeded; False for a non-existent name."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import os\n"
                  b"print(os.path.exists('hello.txt'))\n"
                  b"print(os.path.exists('nope.txt'))\n"
                  b"print(os.path.isfile('hello.txt'))\n"
                  b"\x04"
              ),
              vfiles_init={b"hello.txt": b"hi"},
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "hello.txt'))\nTrue\n" in res.stdout, (
        f"expected True for seeded file, got: {res.stdout!r}"
    )
    assert "nope.txt'))\nFalse\n" in res.stdout, (
        f"expected False for absent file, got: {res.stdout!r}"
    )
    assert "isfile('hello.txt'))\nTrue\n" in res.stdout, (
        f"expected isfile True, got: {res.stdout!r}"
    )


def test_micropython_os_path_getsize_isabs_abspath(micropython_bin: Path) -> None:
    """`os.path.getsize` (stat-backed), `os.path.isabs` (string check
    for sep prefix or drive letter), `os.path.abspath` (prefixes
    getcwd() if relative + dedups separators)."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import os\n"
                  b"print(os.path.getsize('data.bin'))\n"
                  b"print(os.path.isabs('foo'))\n"
                  b"print(os.path.isabs('\\\\foo'))\n"
                  b"print(os.path.isabs('C:\\\\foo'))\n"
                  b"print(os.path.abspath('foo.txt'))\n"
                  b"print(os.path.abspath('C:\\\\dos\\\\app.exe'))\n"
                  b"\x04"
              ),
              vfiles_init={b"data.bin": b"0123456789"},  # 10 bytes
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    # getsize
    assert "\n10\n" in res.stdout, f"getsize: {res.stdout!r}"
    # isabs: relative → False, sep-prefixed or drive-prefixed → True
    assert "isabs('foo'))\nFalse\n" in res.stdout, (
        f"isabs relative: {res.stdout!r}"
    )
    assert "\\\\foo'))\nTrue\n" in res.stdout, (
        f"isabs sep-prefix: {res.stdout!r}"
    )
    assert "C:\\\\foo'))\nTrue\n" in res.stdout, (
        f"isabs drive: {res.stdout!r}"
    )
    # abspath: relative gets cwd-prefixed (dos_emu's synthetic cwd
    # is "C:\\"); already-absolute passes through.
    assert "C:\\foo.txt" in res.stdout, f"abspath relative: {res.stdout!r}"
    assert "C:\\dos\\app.exe" in res.stdout, (
        f"abspath absolute: {res.stdout!r}"
    )


def test_micropython_os_path_normpath(micropython_bin: Path) -> None:
    """`os.path.normpath` collapses redundant separators and
    canonicalizes forward-slash → backslash. Doesn't resolve `..`
    (DOS conventions vary, conservative)."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import os\n"
                  b"print(os.path.normpath('a/b//c'))\n"
                  b"print(os.path.normpath('C:/dos/\\\\foo'))\n"
                  b"print(os.path.normpath(''))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "a\\b\\c" in res.stdout, f"normpath dedup: {res.stdout!r}"
    assert "C:\\dos\\foo" in res.stdout, f"normpath slashes: {res.stdout!r}"
    assert "\n.\n" in res.stdout, f"normpath empty→dot: {res.stdout!r}"


def test_micropython_sys_exc_info(micropython_bin: Path) -> None:
    """`sys.exc_info()` returns the (type, value, tb) tuple inside
    an except block, and (None, None, None) outside. Default OFF
    in mpconfig.h — opted in via MICROPY_PY_SYS_EXC_INFO."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import sys\n"
                  b"try:\n"
                  b"    1/0\n"
                  b"except ZeroDivisionError:\n"
                  b"    info = sys.exc_info()\n"
                  b"    print(info[0].__name__, str(info[1]))\n"
                  b"\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "ZeroDivisionError divide by zero" in res.stdout, (
        f"expected exc_info type+message, got: {res.stdout!r}"
    )


def test_micropython_range_binop(micropython_bin: Path) -> None:
    """`range(3) == range(3)` should return True now that
    MICROPY_PY_BUILTINS_RANGE_BINOP is on (default-off below the
    EVERYTHING ROM level). Without it ranges only compare equal
    by identity — a frequent CPython-portability surprise."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"print(range(3) == range(3))\n"
                  b"print(range(0, 3) == range(3))\n"
                  b"print(range(3) == range(4))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    # CPython behavior: equal-by-value, including normalized step.
    # REPL echoes `>>> ` between each print so the True/False lines
    # don't sit consecutively in stdout — split on prompts and pick
    # the prints' immediate next lines.
    assert "== range(3))\nTrue\n" in res.stdout, (
        f"expected range(3) == range(3) → True, got: {res.stdout!r}"
    )
    assert "range(0, 3) == range(3))\nTrue\n" in res.stdout, (
        f"expected range(0,3) == range(3) → True, got: {res.stdout!r}"
    )
    assert "== range(4))\nFalse\n" in res.stdout, (
        f"expected range(3) == range(4) → False, got: {res.stdout!r}"
    )


def test_micropython_hashlib_md5(micropython_bin: Path) -> None:
    """`hashlib.md5(b"abc").hexdigest()` should equal RFC 1321's
    canonical test vector. Backed by B-Con's public-domain md5.c
    (fetched into upstream/lib/crypto-algorithms/) routed through
    uc386-dos/lib/axtls/crypto/crypto.h's AXTLS-shaped shim."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import hashlib\n"
                  b"print(hashlib.md5(b'abc').digest().hex())\n"
                  b"print(hashlib.md5(b'').digest().hex())\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    # md5("abc") = 900150983cd24fb0d6963f7d28e17f72 (RFC 1321 test).
    assert "900150983cd24fb0d6963f7d28e17f72" in res.stdout, (
        f"expected RFC 1321 md5(b'abc') vector, got: {res.stdout!r}"
    )
    # md5("") = d41d8cd98f00b204e9800998ecf8427e.
    assert "d41d8cd98f00b204e9800998ecf8427e" in res.stdout, (
        f"expected md5('') canonical vector, got: {res.stdout!r}"
    )


def test_micropython_hashlib_sha1(micropython_bin: Path) -> None:
    """`hashlib.sha1(b"abc").hexdigest()` should equal FIPS 180-1's
    canonical test vector. Same path as md5 — B-Con's sha1.c routed
    via the AXTLS-shaped crypto.h shim."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import hashlib\n"
                  b"print(hashlib.sha1(b'abc').digest().hex())\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    # sha1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d (FIPS 180-1).
    assert "a9993e364706816aba3e25717850c26c9cd0d89d" in res.stdout, (
        f"expected FIPS 180-1 sha1(b'abc') vector, got: {res.stdout!r}"
    )


def test_micropython_json_roundtrip(micropython_bin: Path) -> None:
    """`json.loads` + `json.dumps` round-trip. Backed by upstream's
    `extmod/modjson.c`, gated on MICROPY_PY_JSON (default 1 at
    EXTRA_FEATURES). Pulled into the build alongside modplatform.c
    in the same slice that adds these tests."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import json\n"
                  b"d = json.loads('{\"a\": 1, \"b\": [2, 3]}')\n"
                  b"print(d['a'], d['b'])\n"
                  b"print(json.dumps({'k': 'v'}))\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "1 [2, 3]" in res.stdout, (
        f"expected parsed JSON values, got: {res.stdout!r}"
    )
    assert '{"k": "v"}' in res.stdout, (
        f"expected JSON dump, got: {res.stdout!r}"
    )


def test_micropython_platform_module(micropython_bin: Path) -> None:
    """`import platform; platform.platform()` returns a descriptive
    string built from MICROPY_PLATFORM_* defines. Backed by
    upstream's `extmod/modplatform.c`. We pin the version stamp
    set in mpconfigport.h so platform.platform() is recognizable
    rather than a generic 'MicroPython'."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import platform\n"
                  b"print(platform.platform())\n"
                  b"print(platform.machine())\n"
                  b"\x04"
              ),
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    # MICROPY_PLATFORM_VERSION = "uc386-dos i386 (PMODE/W)" lands
    # in the platform.platform() string verbatim.
    assert "uc386-dos" in res.stdout, (
        f"expected uc386-dos platform marker, got: {res.stdout!r}"
    )
    assert "x86" in res.stdout, (
        f"expected x86 machine marker (from __i386__ detection), "
        f"got: {res.stdout!r}"
    )


def test_micropython_os_system_exec(micropython_bin: Path) -> None:
    """`os.system(cmd)` invokes the DOS shell via INT 21h AH=0x4B
    sub 0 (LOAD AND EXECUTE), then reads the child's exit code via
    AH=0x4D. dos_emu's tiny built-in interpreter recognizes ECHO
    (writes args to stdout, exit 0) and EXIT N (returns N as the
    child exit code). Real DOS would shell out through COMMAND.COM
    via the same call."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import os\n"
                  b"rc = os.system('ECHO hello world')\n"
                  b"print('echo rc=', rc)\n"
                  b"rc = os.system('EXIT 7')\n"
                  b"print('exit rc=', rc)\n"
                  b"\x04"
              ),
              env={"COMSPEC": "C:\\COMMAND.COM"},
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "hello world" in res.stdout, (
        f"expected ECHO output, got: {res.stdout!r}"
    )
    assert "echo rc= 0" in res.stdout, (
        f"expected ECHO exit 0, got: {res.stdout!r}"
    )
    assert "exit rc= 7" in res.stdout, (
        f"expected EXIT 7 to return 7, got: {res.stdout!r}"
    )


def test_micropython_lwip_loopback_tcp(micropython_bin: Path) -> None:
    """`import lwip; lwip.reset()` initializes lwIP's IPv4 stack.
    Then a server `socket(AF_INET, SOCK_STREAM)` binds 127.0.0.1:8080
    and listens, a client socket connects (EINPROGRESS on
    non-blocking), `lwip.callback()` drives `uc386dos_loopback_poll`
    + `sys_check_timeouts` to drain the SYN/SYN-ACK/ACK on the
    `loop_netif` queue, accept returns the new conn, send writes
    10 bytes, and after another callback round-trip, the server's
    recv reads back exactly `b'hello lwip'`. End-to-end TCP/IP
    round-trip on FreeDOS via lwIP STABLE-2_2_1, no real net.

    Guards against three regressions:
    1. `lwip-arch-cc.h` PACK_STRUCT_STRUCT must expand to
       `__attribute__((packed))` — otherwise lwIP's init.c
       boot-time `LWIP_ASSERT(sizeof(packed_struct_test) == 5)`
       aborts on a u8+u32 layout that comes out 8 bytes unpacked.
    2. `mod_lwip_reset` must register `uc386dos_loopback_poll`
       (the netif_poll_all driver) — fetch.sh's idempotent patch
       to upstream/extmod/modlwip.c.
    3. `uc386dos_loopback_poll` must call `netif_poll_all()` not
       `netif_poll(netif_default)` — `netif_default` stays NULL
       because netif_init doesn't promote `loop_netif`."""
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import lwip\n"
                  b"lwip.reset()\n"
                  b"import socket\n"
                  b"srv = socket.socket()\n"
                  b"srv.bind((\"127.0.0.1\", 8080))\n"
                  b"srv.listen(1)\n"
                  b"srv.setblocking(False)\n"
                  b"cli = socket.socket()\n"
                  b"cli.setblocking(False)\n"
                  b"try:\n"
                  b"    cli.connect((\"127.0.0.1\", 8080))\n"
                  b"except OSError:\n"
                  b"    pass\n"
                  b"\n"
                  b"for _ in range(5):\n"
                  b"    lwip.callback()\n"
                  b"\n"
                  b"conn, addr = srv.accept()\n"
                  b"conn.setblocking(False)\n"
                  b"print('accepted')\n"
                  b"n = cli.send(b'hello lwip')\n"
                  b"print('sent', n)\n"
                  b"\n"
                  b"for _ in range(5):\n"
                  b"    lwip.callback()\n"
                  b"\n"
                  b"data = conn.recv(64)\n"
                  b"print('recv:', data)\n"
                  b"\x04"
              ),
              timeout_seconds=30.0,
              instruction_limit=8_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    # No abort-on-assert means the packing fix held.
    assert "lwIP assert:" not in res.stdout, (
        f"lwIP assertion fired during init: {res.stdout!r}"
    )
    assert "accepted" in res.stdout, (
        f"server accept didn't complete (handshake stalled?): "
        f"{res.stdout!r}"
    )
    assert "sent 10" in res.stdout, (
        f"client send didn't ship 10 bytes: {res.stdout!r}"
    )
    assert "recv: b'hello lwip'" in res.stdout, (
        f"server recv didn't see the payload: {res.stdout!r}"
    )


def _run_dhcp(micropython_bin: Path, *, crynwr_int_num):
    """Shared driver: bring up eth, run DHCP discovery, return Result + sim."""
    from uc386.harness import run
    from uc386.dos_emu_netsim import NetworkSimulator

    net = NetworkSimulator(crynwr_int_num=crynwr_int_num)
    res = run(micropython_bin,
              stdin_bytes=(
                  b"import lwip, uc386_net\n"
                  b"lwip.reset()\n"
                  b"print('init rc:', uc386_net.eth_init(True))\n"
                  b"for _ in range(40):\n"
                  b"    lwip.callback()\n"
                  b"\n"
                  b"print('status:', uc386_net.eth_status())\n"
                  b"\x04"
              ),
              net=net,
              timeout_seconds=30.0,
              instruction_limit=8_000_000_000)
    return res, net


def test_micropython_lwip_dhcp(micropython_bin: Path) -> None:
    """End-to-end DHCP over the Crynwr packet-driver path. With the
    simulator's default `crynwr_int_num=0x60`, dos_emu plants the
    "PKT DRVR" signature in low memory and answers DPMI INT 0x31
    fn 0x0200 + the AH=01/02/04/06 Crynwr server functions, so the
    binary's `pktdrv_init` succeeds and the eth netif uses the
    Crynwr send path. Receive still rides on INT 0x83 (the DPMI
    receiver thunk for real DOS is a documented follow-up); both
    paths funnel through the same NetworkSimulator under emulation.
    """
    res, net = _run_dhcp(micropython_bin, crynwr_int_num=0x60)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "init rc: 0" in res.stdout, (
        f"eth_init didn't succeed (return 0): {res.stdout!r}"
    )
    assert "10.0.2.15" in res.stdout, (
        f"DHCP-offered IP 10.0.2.15 not visible in status: {res.stdout!r}"
    )
    assert "10.0.2.2" in res.stdout, (
        f"DHCP gateway 10.0.2.2 not visible in status: {res.stdout!r}"
    )
    assert "'pktdrv'" in res.stdout, (
        f"Driver should report 'pktdrv' under Crynwr mode: {res.stdout!r}"
    )
    assert net.pktdrv_receiver_addr != 0, (
        "pktdrv access_type didn't register the receiver address"
    )
    # Sanity on the simulator side: at least one DHCP DISCOVER must
    # have been transmitted (op=1 BOOTREQUEST in a UDP 68->67 frame).
    saw_discover = False
    for f in net.tx_log:
        if len(f) < 14 + 20 + 8 + 240:
            continue
        if f[12:14] != b"\x08\x00":  # not IPv4
            continue
        if f[14 + 9] != 17:  # not UDP
            continue
        sport = (f[14 + 20] << 8) | f[14 + 20 + 1]
        dport = (f[14 + 20 + 2] << 8) | f[14 + 20 + 3]
        if sport == 68 and dport == 67:
            saw_discover = True
            break
    assert saw_discover, (
        "no DHCP client packet (UDP 68->67) was transmitted: "
        f"{len(net.tx_log)} frames in tx_log"
    )
    # The binary should have exercised DPMI INT 31h fn 0x0303 to
    # register a real-mode callback for the Crynwr receiver — even
    # though under emulation the actual delivery rides AH=0x99
    # polling, the registration call itself is the real-DOS shape
    # we want to keep wired and tested.
    assert len(net.dpmi_callbacks) >= 1, (
        "DPMI fn 0x0303 (Allocate Real Mode Callback) wasn't called"
    )
    handler_linear, rmcs_linear, _seg, _off = net.dpmi_callbacks[0]
    assert handler_linear != 0, "DPMI handler address was zero"
    assert rmcs_linear != 0, "DPMI RMCS address was zero"


def test_micropython_lwip_dhcp_int83_fallback(micropython_bin: Path) -> None:
    """Same DHCP flow but with the simulator's Crynwr support disabled
    (`crynwr_int_num=None`). The IVT scan should fail (no signature
    planted, DPMI fn 0x0200 returns CF=1), `pktdrv_init` falls back
    to the INT 0x83 sim, and `eth_status()` reports `int83-sim` as
    the active driver. Confirms the autodetect ladder doesn't lock
    us into Crynwr-only on environments where it isn't wired."""
    res, _net = _run_dhcp(micropython_bin, crynwr_int_num=None)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "10.0.2.15" in res.stdout, (
        f"DHCP-offered IP 10.0.2.15 not visible in status: {res.stdout!r}"
    )
    assert "'int83-sim'" in res.stdout, (
        f"Driver should fall back to 'int83-sim' "
        f"when Crynwr is absent: {res.stdout!r}"
    )


def test_micropython_lwip_dns_query(micropython_bin: Path) -> None:
    """`socket.getaddrinfo("echo.test", 80)` should resolve through
    lwIP's DNS client against the simulator's DNS responder.

    Path under test:
      uc386_net.eth_init(True) → DHCP gives DNS=10.0.2.3
      socket.getaddrinfo  → modlwip → lwIP dns_gethostbyname
                          → UDP 53 query to 10.0.2.3
                          → NetworkSimulator._handle_dns
                          → A-record response with `echo.test` IP
      lwIP DNS client    → callback fires → state.status=1
      modlwip            → returns [(AF_INET, ..., (ip, port))]
      uc386 binary       → prints the resolved address

    Pins three pieces:
    1. mpconfigport.h `MICROPY_PY_LWIP_POLL_HOOK` actually drives
       `uc386dos_loopback_poll` + `sys_check_timeouts` so blocking
       calls (the DNS resolution loop in modlwip's poll_sockets())
       can drain inbound UDP responses.
    2. NetworkSimulator's ARP responder answers for `dns_ip`, not
       just the gateway, so lwIP's DNS query reaches the sim.
    3. The DNS handler decodes the question, looks up the A record,
       and builds a spec-shaped reply that lwIP's resolver accepts.
    """
    from uc386.harness import run
    from uc386.dos_emu_netsim import NetworkSimulator

    net = NetworkSimulator()
    res = run(micropython_bin,
              stdin_bytes=(
                  b"import lwip, uc386_net, socket\n"
                  b"lwip.reset()\n"
                  b"uc386_net.eth_init(True)\n"
                  b"for _ in range(40):\n"
                  b"    lwip.callback()\n"
                  b"\n"
                  b"info = socket.getaddrinfo('echo.test', 80)\n"
                  b"print('addr:', info[0][4])\n"
                  b"\x04"
              ),
              net=net,
              timeout_seconds=30.0,
              instruction_limit=8_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "10.0.2.50" in res.stdout, (
        f"DNS resolution didn't return 10.0.2.50 for echo.test: "
        f"{res.stdout!r}"
    )
    # The simulator should have observed at least one UDP packet
    # to port 53 (DNS query).
    saw_dns = False
    for f in net.tx_log:
        if len(f) < 14 + 20 + 8 + 12:
            continue
        if f[12:14] != b"\x08\x00":  # not IPv4
            continue
        if f[14 + 9] != 17:  # not UDP
            continue
        dport = (f[14 + 20 + 2] << 8) | f[14 + 20 + 3]
        if dport == 53:
            saw_dns = True
            break
    assert saw_dns, (
        "no DNS query (UDP dport=53) was transmitted: "
        f"{len(net.tx_log)} frames in tx_log"
    )


def test_micropython_select_poll(micropython_bin: Path) -> None:
    """`select.poll()` should multiplex non-blocking sockets and pick
    out the one that's readable.

    Sets up a loopback server, creates a poller registered for read
    events on it, drives `lwip.callback()` to advance the loopback
    handshake while a client connects + sends payload bytes, then
    asserts the poller fires for the accepted connection's socket.

    Pins:
    1. select.poll() builds and `register/unregister` work.
    2. modlwip's socket implements the stream poll protocol so
       MP_STREAM_POLL_RD fires when data lands.
    3. POLL_HOOK keeps draining loop_netif during poll waits.
    """
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import lwip, select, socket\n"
                  b"lwip.reset()\n"
                  b"srv = socket.socket()\n"
                  b"srv.bind(('127.0.0.1', 9100))\n"
                  b"srv.listen(1)\n"
                  b"srv.setblocking(False)\n"
                  b"cli = socket.socket()\n"
                  b"cli.setblocking(False)\n"
                  b"try:\n"
                  b"    cli.connect(('127.0.0.1', 9100))\n"
                  b"except OSError:\n"
                  b"    pass\n"
                  b"\n"
                  b"for _ in range(5):\n"
                  b"    lwip.callback()\n"
                  b"\n"
                  b"conn, _ = srv.accept()\n"
                  b"conn.setblocking(False)\n"
                  b"poller = select.poll()\n"
                  b"poller.register(conn, select.POLLIN)\n"
                  b"print('idle:', poller.poll(0))\n"
                  b"cli.send(b'ping')\n"
                  b"for _ in range(5):\n"
                  b"    lwip.callback()\n"
                  b"\n"
                  b"events = poller.poll(0)\n"
                  b"print('ready:', len(events), 'flags=', events[0][1] if events else None)\n"
                  b"print('payload:', conn.recv(16))\n"
                  b"\x04"
              ),
              timeout_seconds=20.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "idle: []" in res.stdout, (
        f"poll(0) on idle conn should be empty: {res.stdout!r}"
    )
    assert "ready: 1" in res.stdout, (
        f"poll(0) didn't fire after data arrived: {res.stdout!r}"
    )
    assert "payload: b'ping'" in res.stdout, (
        f"recv didn't return the payload: {res.stdout!r}"
    )


def test_micropython_lwip_udp_socket(micropython_bin: Path) -> None:
    """`socket(AF_INET, SOCK_DGRAM)` end-to-end on loopback.

    Two UDP sockets bind to different ports on 127.0.0.1; the
    client `sendto`s a datagram, lwIP routes it through the loopback
    netif (which we drain via `lwip.callback()`), and the server
    `recvfrom` returns the payload + source address.

    Pins the UDP path through the public Python `socket` API
    (DHCP and DNS go through internal lwIP code, so this is the
    first SOCK_DGRAM exercise via modlwip's send/recv glue).
    """
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import lwip, socket\n"
                  b"lwip.reset()\n"
                  b"a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)\n"
                  b"a.bind(('127.0.0.1', 9201))\n"
                  b"a.setblocking(False)\n"
                  b"b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)\n"
                  b"b.bind(('127.0.0.1', 9202))\n"
                  b"b.setblocking(False)\n"
                  b"\n"
                  b"sent = b.sendto(b'hello-udp', ('127.0.0.1', 9201))\n"
                  b"print('sent:', sent)\n"
                  b"\n"
                  b"for _ in range(5):\n"
                  b"    lwip.callback()\n"
                  b"\n"
                  b"data, src = a.recvfrom(64)\n"
                  b"print('got:', data, 'from', src)\n"
                  b"\x04"
              ),
              timeout_seconds=20.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "sent: 9" in res.stdout, (
        f"sendto didn't return the payload length: {res.stdout!r}"
    )
    assert "got: b'hello-udp'" in res.stdout, (
        f"recvfrom didn't return the payload: {res.stdout!r}"
    )
    assert "from ('127.0.0.1', 9202)" in res.stdout, (
        f"recvfrom didn't surface the source address: {res.stdout!r}"
    )


def test_micropython_lwip_http_loopback(micropython_bin: Path) -> None:
    """An HTTP-shaped TCP exchange on loopback. Client GETs `/`, server
    accepts, reads the request line + headers, writes back an HTTP/1.0
    200 with a JSON body. Client reads, parses status + body.

    Real-world workload shape: HTTP request lines, header parsing,
    multi-pbuf payloads. Exercises that lwIP's TCP delivers more than
    one segment per direction and that the recv buffer assembly is
    correct under non-blocking I/O.
    """
    from uc386.harness import run

    res = run(micropython_bin,
              stdin_bytes=(
                  b"import lwip, socket\n"
                  b"lwip.reset()\n"
                  b"srv = socket.socket()\n"
                  b"srv.bind(('127.0.0.1', 8000))\n"
                  b"srv.listen(1)\n"
                  b"srv.setblocking(False)\n"
                  b"cli = socket.socket()\n"
                  b"cli.setblocking(False)\n"
                  b"try:\n"
                  b"    cli.connect(('127.0.0.1', 8000))\n"
                  b"except OSError:\n"
                  b"    pass\n"
                  b"\n"
                  b"for _ in range(5):\n"
                  b"    lwip.callback()\n"
                  b"\n"
                  b"conn, _ = srv.accept()\n"
                  b"conn.setblocking(False)\n"
                  b"req = b'GET / HTTP/1.0\\r\\nHost: 127.0.0.1\\r\\n\\r\\n'\n"
                  b"cli.send(req)\n"
                  b"for _ in range(5):\n"
                  b"    lwip.callback()\n"
                  b"\n"
                  b"got = conn.recv(256)\n"
                  b"print('req-len:', len(got))\n"
                  b"print('verb:', got.split(b' ')[0])\n"
                  b"\n"
                  b"resp = b'HTTP/1.0 200 OK\\r\\nContent-Type: application/json\\r\\nContent-Length: 13\\r\\n\\r\\n{\"ok\":true}\\r\\n'\n"
                  b"conn.send(resp)\n"
                  b"for _ in range(5):\n"
                  b"    lwip.callback()\n"
                  b"\n"
                  b"reply = cli.recv(512)\n"
                  b"head, _, body = reply.partition(b'\\r\\n\\r\\n')\n"
                  b"print('status:', head.split(b'\\r\\n')[0])\n"
                  b"print('body:', body.strip())\n"
                  b"\x04"
              ),
              timeout_seconds=20.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert res.exit_code == 0
    assert "verb: b'GET'" in res.stdout, (
        f"server didn't see the GET verb: {res.stdout!r}"
    )
    assert "status: b'HTTP/1.0 200 OK'" in res.stdout, (
        f"client didn't parse the status line: {res.stdout!r}"
    )
    assert b'{"ok":true}'.decode() in res.stdout, (
        f"client didn't see the JSON body: {res.stdout!r}"
    )


def test_dosbox_x_rig_loads_ne2000(tmp_path: Path) -> None:
    """The dosbox-x-rig should boot DOSBox-X cleanly with our config
    and load the Crynwr NE2000 packet driver at INT 0x60. Skipped
    when dosbox-x isn't on PATH (no `brew install dosbox-x`).

    Runs in `-silent` mode (no SDL window) and parses RIG.LOG to
    confirm the packet driver reports the expected install banner.
    """
    import os
    import shutil
    import subprocess

    if shutil.which("dosbox-x") is None:
        pytest.skip("dosbox-x not on PATH; install via `brew install dosbox-x`")
    # The dosbox-x rig lives at `rigs/dosbox-x-rig/` in this repo
    # (since the split out of uc386). Test file is at `tests/test_smoke.py`,
    # so go up one to the repo root, then down into rigs/.
    rig_dir = Path(__file__).resolve().parent.parent / "rigs" / "dosbox-x-rig"
    if not rig_dir.is_dir():
        pytest.skip(f"rig directory missing: {rig_dir}")
    if not (rig_dir / "NE2000.COM").is_file():
        # fetch.sh idempotently retrieves the packet driver from
        # archive.org. Do it inline so a CI runner without a
        # pre-fetched binary still passes.
        rc = subprocess.run([str(rig_dir / "fetch.sh")],
                            capture_output=True, text=True)
        assert rc.returncode == 0, (
            f"fetch.sh failed: {rc.stderr}"
        )
    log_path = rig_dir / "RIG.LOG"
    if log_path.exists():
        log_path.unlink()
    # Drop into the rig dir so DOSBox-X resolves dosbox-x.conf and
    # the autoexec's `mount C .` lands on the right directory.
    res = subprocess.run(
        ["dosbox-x", "-silent", "-conf", "dosbox-x.conf"],
        cwd=str(rig_dir),
        capture_output=True, text=True,
        timeout=60,
    )
    assert log_path.exists(), (
        f"RIG.LOG not produced. dosbox-x stderr: {res.stderr[-500:]}"
    )
    log = log_path.read_text(errors="replace")
    assert "Packet driver for NE2000" in log, (
        f"NE2000 driver didn't load: {log}"
    )
    assert "software interrupt is 0x60" in log, (
        f"NE2000 didn't install at INT 0x60: {log}"
    )
    assert "I/O port 0x300" in log, (
        f"NE2000 didn't bind I/O 0x300: {log}"
    )


# ---------------------------------------------------------------------------
# `ssl` / `tls` module — extmod/modtls_axtls.c bridged to upstream/lib/axtls/.
#
# Real TLS handshakes need a server somewhere (and lwIP routed to it via
# NE2000+SLIRP under QEMU). These tests cover the API surface that's
# exercisable without a network: import, SSLContext construction, attribute
# defaults. End-to-end handshake against `python -m http.server` over TLS
# is in the QEMU+FreeDOS rig under dosbox-x-rig/run-qemu-freedos.sh.
# ---------------------------------------------------------------------------


def test_micropython_import_ssl(micropython_bin: Path) -> None:
    """`import ssl` should resolve to the modtls_axtls.c-backed module
    (registered as both `tls` and `ssl` aliases in moduledefs.h).
    The module exposes `SSLContext`, `PROTOCOL_TLS_CLIENT`, and
    `CERT_NONE`.
    """
    from uc386.harness import run

    program = (
        b"import ssl\n"
        b"print('SSL_MARK', hasattr(ssl, 'SSLContext'),"
        b" ssl.PROTOCOL_TLS_CLIENT, ssl.CERT_NONE)\n"
        b"\x04"
    )
    res = run(micropython_bin,
              stdin_bytes=program,
              timeout_seconds=20.0,
              instruction_limit=8_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert "SSL_MARK True 0 0" in res.stdout, (
        f"`import ssl` smoke failed: {res.stdout!r}"
    )


def test_micropython_ssl_context_construct(micropython_bin: Path) -> None:
    """SSLContext(PROTOCOL_TLS_CLIENT) should construct without
    raising; verify_mode defaults to CERT_NONE."""
    from uc386.harness import run

    program = (
        b"import ssl\n"
        b"ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)\n"
        b"print('CTX_MARK', ctx.verify_mode == ssl.CERT_NONE)\n"
        b"\x04"
    )
    res = run(micropython_bin,
              stdin_bytes=program,
              timeout_seconds=20.0,
              instruction_limit=8_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert "CTX_MARK True" in res.stdout, (
        f"SSLContext construction failed: {res.stdout!r}"
    )


def test_micropython_ssl_verify_mode_settable(micropython_bin: Path) -> None:
    """verify_mode is settable to CERT_REQUIRED; reads back the
    stored value. Regression for the upstream-divergence in our
    vendored modtls_axtls_uc386dos.c (upstream hard-codes the read
    to CERT_NONE; we honor the field)."""
    from uc386.harness import run

    program = (
        b"import ssl\n"
        b"ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)\n"
        b"ctx.verify_mode = ssl.CERT_REQUIRED\n"
        b"print('VFY_MARK', ctx.verify_mode == ssl.CERT_REQUIRED)\n"
        b"\x04"
    )
    res = run(micropython_bin,
              stdin_bytes=program,
              timeout_seconds=20.0,
              instruction_limit=8_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert "VFY_MARK True" in res.stdout, (
        f"verify_mode round-trip failed: {res.stdout!r}"
    )


# Mozilla CA root: ISRG Root X1 (Let's Encrypt). Small, well-known,
# expires 2035 — long-lived enough that this test won't decay quickly.
# axtls parses PEM by stripping the BEGIN/END markers and base64-decoding
# the body; the inner DER must be valid X.509.
ISRG_ROOT_X1_PEM = b"""\
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
"""


def test_micropython_ssl_load_verify_locations(micropython_bin: Path) -> None:
    """load_verify_locations(cadata=...) should accept a real
    PEM-encoded CA root (ISRG Root X1). Exercises axtls's
    ssl_obj_memory_load(SSL_OBJ_X509_CACERT, ...) — gated on
    CONFIG_SSL_HAS_PEM=1 + CONFIG_SSL_CERT_VERIFICATION=1, both
    flipped by fetch.sh's patch_axtls_config_verify hook."""
    from uc386.harness import run

    pem_bytes = ISRG_ROOT_X1_PEM
    program = (
        b"import ssl\n"
        b"ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)\n"
        b"ctx.verify_mode = ssl.CERT_REQUIRED\n"
        b"pem = " + repr(pem_bytes).encode("ascii") + b"\n"
        b"ctx.load_verify_locations(cadata=pem)\n"
        b"print('CA_MARK', ctx.verify_mode == ssl.CERT_REQUIRED)\n"
        b"\x04"
    )
    res = run(micropython_bin,
              stdin_bytes=program,
              timeout_seconds=30.0,
              instruction_limit=20_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert "CA_MARK True" in res.stdout, (
        f"load_verify_locations failed: {res.stdout!r}"
    )


def test_micropython_select_module_imports(micropython_bin: Path) -> None:
    """`import select` should expose `select.poll()` and the standard
    poll constants. Gates MICROPY_PY_SELECT + the modselect.c TU in
    build_port.sh. We don't register any pollable streams here — just
    constructing the poller and checking the type/constants is enough
    to pin the module-registration path.
    """
    from uc386.harness import run

    program = (
        b"import select\n"
        b"p = select.poll()\n"
        b"print(type(p).__name__)\n"
        b"print(select.POLLIN, select.POLLOUT)\n"
        b"\x04\x04"
    )
    res = run(micropython_bin,
              stdin_bytes=program,
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert "poll" in res.stdout, (
        f"select.poll() didn't return a poll-like object: {res.stdout!r}"
    )
    assert "1 4" in res.stdout, (
        f"select.POLLIN/POLLOUT constants missing or wrong (expected "
        f"`1 4`): {res.stdout!r}"
    )


def test_micropython_asyncio_taskqueue(micropython_bin: Path) -> None:
    """`import _asyncio` exposes the C-side TaskQueue + Task primitives
    that the user-facing asyncio Python wrapper imports from. Gates
    MICROPY_PY_ASYNCIO + the modasyncio.c TU in build_port.sh.

    The full `import asyncio` path needs the upstream extmod/asyncio/*.py
    files shipped to the DOS image — not in scope for this smoke. Test
    only the C layer.
    """
    from uc386.harness import run

    program = (
        b"import _asyncio\n"
        b"q = _asyncio.TaskQueue()\n"
        b"print(type(q).__name__)\n"
        b"print(q.peek() is None)\n"
        b"\x04\x04"
    )
    res = run(micropython_bin,
              stdin_bytes=program,
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert "TaskQueue" in res.stdout, (
        f"_asyncio.TaskQueue() didn't return a TaskQueue: {res.stdout!r}"
    )
    assert "True" in res.stdout, (
        f"empty TaskQueue.peek() didn't return None: {res.stdout!r}"
    )


def test_micropython_machine_module(micropython_bin: Path) -> None:
    """`import machine` should expose `machine.mem8/16/32`, `idle()`, and
    `Signal`. The main DOS-useful feature is mem8/16/32 — under
    DOS/32A's flat DS, linear addresses 0..4G are mapped 1:1 so
    `machine.mem16[0x410]` reads the BIOS data area's equipment-list
    word (a stable, non-zero value on every PC). Pins:
        1. module registers (`machine` in builtins).
        2. machine_mem.c links and the subscript path works.
        3. idle() is a no-op that returns cleanly (our port glue stub).

    The dos_emu rig doesn't actually populate the BIOS data area (we
    run a flat .bin in an emulator with no BIOS), so the read returns
    0; we just assert it didn't crash and produced an integer.
    """
    from uc386.harness import run

    program = (
        b"import machine\n"
        b"print(type(machine.mem8).__name__)\n"
        b"print(type(machine.mem16).__name__)\n"
        b"print(type(machine.mem32).__name__)\n"
        b"machine.idle()\n"
        b"print('idle_ok')\n"
        b"v = machine.mem16[0x410]\n"
        b"print('mem16:', type(v).__name__)\n"
        b"\x04\x04"
    )
    res = run(micropython_bin,
              stdin_bytes=program,
              timeout_seconds=15.0,
              instruction_limit=4_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert "mem" in res.stdout, (
        f"machine.mem8/16/32 missing: {res.stdout!r}"
    )
    assert "idle_ok" in res.stdout, (
        f"machine.idle() didn't return: {res.stdout!r}"
    )
    assert "mem16: int" in res.stdout, (
        f"machine.mem16[0x410] didn't produce an int: {res.stdout!r}"
    )


def test_micropython_ssl_load_verify_locations_requires_cadata(micropython_bin: Path) -> None:
    """load_verify_locations() with no cadata raises TypeError — our
    modtls glue requires explicit data (cafile= isn't supported in
    DOS without a VFS bridge to axtls). Exercises the parameter
    validation path before any axtls call.
    """
    from uc386.harness import run

    program = (
        b"import ssl\n"
        b"ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)\n"
        b"try:\n"
        b"    ctx.load_verify_locations()\n"
        b"    print('NO_RAISE')\n"
        b"except (TypeError, OSError):\n"
        b"    print('RAISED_OK')\n"
        b"\n"
        b"\x04\x04"
    )
    res = run(micropython_bin,
              stdin_bytes=program,
              timeout_seconds=20.0,
              instruction_limit=8_000_000_000)
    assert not res.timed_out, "REPL didn't exit"
    assert res.error is None, f"dos_emu reported error: {res.error}"
    assert "RAISED_OK" in res.stdout, (
        f"missing-cadata didn't raise: {res.stdout!r}"
    )
