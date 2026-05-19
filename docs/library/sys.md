---
title: sys
---

# `sys` — system parameters

```python
import sys
```

## Attributes

### `sys.argv` — command-line arguments

A `list[str]` of arguments passed to the program. On this port,
`sys.argv` starts out as `['']` (no real argv is plumbed through
the PMODE/W stub yet) — you can append to it from inside your
script if you need to thread arguments through `exec()`:

```python
sys.argv = ['script.py', 'arg1', 'arg2']
```

### `sys.path` — module search path

A `list[str]` of directories searched on `import`. Defaults to
`['']` (current directory only). Append to add more search roots:

```python
sys.path.append('C:\\PYLIB')
import mylib       # found at C:\PYLIB\MYLIB.PY (DOS case-folds)
```

### `sys.modules` — cache of imported modules

A `dict` of every module ever imported. Useful for unloading:

```python
import sys, mymod
del sys.modules['mymod']        # next `import mymod` will re-execute
```

### `sys.stdin` / `sys.stdout` / `sys.stderr`

The three standard streams as file-like objects. On DOS they map
to handles 0/1/2 (the DOS console by default, or whatever `CTTY`
redirected them to).

```python
sys.stdout.write('no newline')
sys.stderr.write('warning!\n')
line = sys.stdin.readline()
```

### `sys.platform` — `'fdmp'`

Identifies this port. Use it to write code that's only run on
FreeDOS:

```python
if sys.platform == 'fdmp':
    # DOS-specific path
    ...
```

### `sys.implementation` — version + capability info

A named tuple:

```python
>>> sys.implementation
(name='micropython', version=(1, 26, 0), _machine='uc386-dos with i386', _mpy=4358)
```

### `sys.version` — MicroPython version string

```python
>>> sys.version
'3.4.0; MicroPython v1.26.0'
```

### `sys.byteorder` — `'little'`

i386 is little-endian.

### `sys.maxsize` — largest small-int

Used in places where you'd want a sentinel like "infinity in
integer terms". On i386 PMODE/W: `2**30 - 1` (small ints are
30-bit; bigger uses heap allocation).

## Functions

### `sys.exit(code=0)`

Raises `SystemExit(code)`, which (if uncaught) terminates the
program. `code` should be a small integer; it's returned to DOS
via INT 21h AH=0x4C and surfaces as `%ERRORLEVEL%`.

```python
import sys
sys.exit(0)                # success
sys.exit(1)                # error
```

### `sys.print_exception(exc, file=sys.stdout)`

Prints the traceback for an exception. Useful in `except`:

```python
try:
    something()
except Exception as e:
    sys.print_exception(e)
```

### `sys.exc_info()`

Returns `(type, value, traceback)` for the currently-handled
exception (or `(None, None, None)` if none).

### `sys.atexit(fn)`

Registers a function to be called on program exit. Mainly used by
libraries that need to flush state.

## Attributes (introspection)

### `sys.tracebacklimit`

Cap traceback depth. Set to a small number on tiny screens:

```python
sys.tracebacklimit = 3
```

### `sys.maxsize`, `sys.maxlong` — see "Attributes" above

---

*Credit: adapted from
[MicroPython sys docs](https://docs.micropython.org/en/latest/library/sys.html)
(MIT, © 2014-2024 Damien P. George and contributors).*
