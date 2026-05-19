---
title: DOS file I/O, paths, line endings
---

# DOS file I/O, paths, 8.3 filenames, line endings

The things that surprise programmers who've only used Python on
Unix or Windows.

## 8.3 filenames

DOS FAT filenames are 8 characters + a 3-character extension. The
filesystem case-folds: `Hello.PY` and `HELLO.py` are the same
file.

In Python, write your paths in uppercase to match what `os.listdir()`
returns:

```python
>>> os.listdir()
['MP.EXE', 'HELLO.PY', 'CACERTS.PEM']
```

You can `open('hello.py')` and it works — DOS case-folds at the
filesystem level — but the `os.path` helpers don't lowercase
their results, so be consistent.

### Long extensions get truncated silently

`config.json` saved to FAT becomes `CONFIG.JSO`. Plan around it:
use `.JSO` or `.CFG` in your own conventions.

## Path separators

DOS uses `\` (backslash). In Python strings, that needs escaping:

```python
path = 'C:\\PYLIB\\MYMODULE.PY'      # ok
path = r'C:\PYLIB\MYMODULE.PY'        # raw string — easier
path = '/c/pylib/MYMODULE.PY'         # FORWARD slash — also works
```

The third form works because DOS internally accepts both. You can
mix freely. `os.path.join` uses `\` on output:

```python
>>> os.path.join('C:\\DOS', 'AUTOEXEC.BAT')
'C:\\DOS\\AUTOEXEC.BAT'
```

## Drive letters

`C:\` is a directory rooted on drive C. `os.getcwd()` returns
something like `'A:\\DOS'`; `os.chdir('C:')` switches drive
without changing the C: cwd (DOS tracks one cwd per drive).

## Line endings

DOS text files use CRLF (`\r\n`). MicroPython's `open(path, 'r')`
in this port returns the bytes as-is — there is no Universal
Newlines translation. If you want lines without the `\r`:

```python
with open('HELLO.TXT', 'r') as f:
    for line in f:
        line = line.rstrip('\r\n')
        print(repr(line))
```

For binary mode (`'rb'`), bytes come through unmodified — no
translation at all.

## INT 21h vs higher-level open

The Python `open()` is built on the port's `dos_int21_open` (INT
21h AH=3D) → `dos_int21_read` (AH=3F) / `dos_int21_write` (AH=40)
→ `dos_int21_close` (AH=3E). The DPMI 0x0301 thunk routes the call
into real mode. See [`dosint21`](library/dosint21.md) for raw
access.

## File modes supported

| Mode | Effect                                                 |
|------|--------------------------------------------------------|
| `'r'`  | text read (no newline translation — see above)       |
| `'rb'` | binary read                                          |
| `'w'`  | write text (truncate); currently maps to AH=3D R/W |
| `'wb'` | write binary (truncate)                              |
| `'a'`  | append (currently maps to R/W; AH=0x6C extended open |
|        | for true truncate/append is a TODO)                  |

⚠️ `'w'` on this port currently uses AH=3D in read-write mode plus
manual truncation. For most use cases that's fine; if you need
guaranteed truncation semantics, use `os.remove(path)` first.

## Sample sizes

- Float / large integer files: fine
- 1 MB file read in 1 KB chunks: ~5 s on a 386SX, ~0.3 s on a
  modern emulated DOS
- Single int 21h call: low microseconds

## See also

- [`dosint21`](library/dosint21.md) — raw INT 21h access
- [`machine`](library/machine.md) — direct memory access
- [`os`](library/os.md) — filesystem helpers (`listdir`, `stat`,
  `remove`, `rename`)
- [`io`](library/io.md) — in-memory streams
