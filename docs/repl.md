---
title: Using the REPL
---

# Using the REPL

The REPL — Read, Evaluate, Print, Loop — is the interactive prompt
you see when you start `MP.EXE` with no script argument.

```
MicroPython uc386-triage on 2026-05-01; uc386-dos with i386
Type "help()" for more information.
>>>
```

Anything you type at `>>>` is compiled and executed immediately.

## Basic use

```
>>> 2 + 2
4
>>> name = 'World'
>>> print('hello, ' + name)
hello, World
>>> import sys
>>> sys.implementation
(name='micropython', version=(1, 26, 0), _machine='uc386-dos with i386', _mpy=4358)
```

Multi-line statements start a continuation prompt `... ` and
require an empty line (or the next line at zero indentation) to
finish:

```
>>> def double(x):
...     return x * 2
...
>>> double(21)
42
```

## Paste mode

When you feed large blocks of code to the REPL (especially from a
script file via redirection or COM1), use **paste mode** so MP
doesn't try to parse line-by-line:

- Press **Ctrl-E** to enter paste mode. The prompt changes to
  `===`.
- Paste / type the full block, including blank lines.
- Press **Ctrl-D** to finish. MP compiles the whole block at once
  and runs it.

```
>>>
paste mode; Ctrl-C to cancel, Ctrl-D to finish
=== def fib(n):
===     if n < 2: return n
===     return fib(n-1) + fib(n-2)
===
=== print([fib(i) for i in range(10)])
===
[0, 1, 1, 2, 3, 5, 8, 13, 21, 34]
>>>
```

The bundled rigs (`rigs/ssh-rig/`, `rigs/tls-rig/`) use paste mode
to feed test scripts into MP over the QEMU/DOSBox serial line.

## Key bindings

The REPL emulates a small subset of GNU readline:

- **Ctrl-A** / **Ctrl-E** — start / end of line
- **Ctrl-B** / **Ctrl-F** — character left / right
- **Ctrl-D** — exit REPL (on an empty line) / finish paste mode
- **Ctrl-C** — interrupt (raises `KeyboardInterrupt`)
- **Ctrl-K** — kill to end of line
- **Ctrl-U** — kill to start of line
- **Backspace** / **Ctrl-H** — delete character to the left
- **Up** / **Down** — history (last entry only, no full ring buffer)

History is not persistent across REPL sessions.

## Running scripts

Three ways to run a Python file:

### a) Pipe into stdin

```
A:\> MP.EXE < HELLO.PY
```

The script runs to completion; MP exits when stdin closes. Output
appears on the same console.

### b) `import` the module

If `HELLO.PY` sits in the current directory, just import it:

```
>>> import HELLO
hello from DOS
```

The first `import` runs the module's top-level statements. Later
imports are no-ops (the module is cached in `sys.modules`).

### c) `exec()` a file's contents

```
>>> with open('HELLO.PY') as f:
...     exec(f.read())
...
hello from DOS
```

Useful when you want the script to run as if pasted at the REPL —
its globals end up in your interactive namespace.

## Stopping a runaway program

Press **Ctrl-C**. MP raises `KeyboardInterrupt` at the next bytecode
instruction. If the program is in a tight C-level loop (e.g.
`time.sleep(60)` or a blocking `socket.recv`), Ctrl-C may have to
wait until the call returns.

If MP itself is wedged (no Ctrl-C response, no prompt), you're
looking at a real bug — file an issue at
<https://github.com/avwohl/freedos_micro_python/issues> with
whatever output you have.

## Exiting

```
>>> import sys
>>> sys.exit()
```

Or press **Ctrl-D** at an empty prompt. MP returns to DOS with the
exit code you passed (default 0).

```
>>> sys.exit(2)
```

returns exit code 2 to the parent (DOS BATCH `IF ERRORLEVEL 2`).

## When something goes wrong

- An unhandled exception prints the traceback and drops you back at
  `>>>`.
- A syntax error prints the line + caret position.
- `sys.tracebacklimit = N` caps traceback depth (handy on a small
  screen).

```
>>> def f(): g()
>>> def g(): raise ValueError('boom')
>>> f()
Traceback (most recent call last):
  File "<stdin>", line 1, in <module>
  File "<stdin>", line 1, in f
  File "<stdin>", line 1, in g
ValueError: boom
>>>
```

---

*Credit: section on key bindings adapted from MicroPython's
[REPL docs](https://docs.micropython.org/en/latest/reference/repl.html)
(MIT, © 2014-2024 Damien P. George and contributors).*
