---
title: io
---

# `io` — file-like streams

```python
import io
```

In-memory streams + the IOBase ABC. `open()` returns objects that
follow this protocol; you can build your own with the same shape.

## `io.StringIO(initial='')` — in-memory text stream

```python
buf = io.StringIO()
buf.write('hello ')
buf.write('world')
buf.getvalue()             # 'hello world'

# Read from one:
buf = io.StringIO('line 1\nline 2\n')
buf.readline()             # 'line 1\n'
buf.read()                 # 'line 2\n'
```

Useful when an API wants a file-like and you want a string. For
example, capturing `print()` output:

```python
import sys, io
saved = sys.stdout
sys.stdout = io.StringIO()
print('captured!')
captured = sys.stdout.getvalue()
sys.stdout = saved
print('was:', repr(captured))
```

## `io.BytesIO(initial=b'')` — same, but bytes

```python
buf = io.BytesIO()
buf.write(b'\x00\x01\xff')
buf.getvalue()             # b'\x00\x01\xff'

buf = io.BytesIO(b'hello')
buf.read(3)                # b'hel'
buf.seek(0)                # back to start
buf.read()                 # b'hello'
```

## `io.IOBase` — base class

Subclass to make your own file-like object. Required methods (any
subset you actually use):

- `.read(n=-1)` / `.write(buf)` / `.close()`
- `.readinto(buf, n=-1)` — read into a pre-allocated buffer
- `.seek(offset, whence=0)` / `.tell()`
- `.readable()`, `.writable()`, `.seekable()` — returns bool

Useful for adapting one stream into another (e.g. a TCP socket
adapted as a line-oriented text stream).

## File objects (returned by `open()`)

In addition to the IOBase methods:

- Iteration yields lines: `for line in f: ...`
- `with` works: `with open(path) as f: ...`
- `.flush()` is a no-op (DOS write doesn't buffer)

See [DOS file I/O](../dos-features.md) for 8.3 filename / line
ending details.

---

*Credit: shape from
[MicroPython io docs](https://docs.micropython.org/en/latest/library/io.html)
(MIT).*
