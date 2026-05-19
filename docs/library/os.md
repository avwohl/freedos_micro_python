---
title: os
---

# `os` — filesystem and environment

```python
import os
```

DOS-specific notes are flagged with ⚠️.

## Filesystem

### `os.listdir(path='.')`

List entries in a directory. Returns a `list[str]`.

```python
>>> os.listdir()
['MP.EXE', 'NE2000.COM', 'HELLO.PY', 'AUTOEXEC.BAT', '.', '..']
>>> os.listdir('C:\\')
['DOS', 'AUTOEXEC.BAT', 'CONFIG.SYS', ...]
```

⚠️ DOS includes the `.` and `..` entries in the directory listing.
Strip them yourself if you don't want them:

```python
files = [f for f in os.listdir() if f not in ('.', '..')]
```

### `os.stat(path)`

Returns a tuple of file info: `(mode, ino, dev, nlink, uid, gid,
size, atime, mtime, ctime)`. Most fields are DOS-meaningless
(ino/uid/gid are 0); `size` and `mtime` are real.

```python
>>> os.stat('HELLO.PY')
(32768, 0, 0, 0, 0, 0, 67, 0, 1747630800, 0)
#   ↑                       ↑
#   mode (S_IFREG)          size in bytes
```

### `os.remove(path)` / `os.unlink(path)`

Delete a file.

### `os.rename(old, new)`

Rename / move a file. On DOS this is INT 21h AH=0x56 and works
within a single drive only.

### `os.mkdir(path)` / `os.rmdir(path)`

Create / remove a directory. Standard DOS rules: empty for rmdir.

### `os.chdir(path)` / `os.getcwd()`

Change / get the current working directory. The DOS current
directory is per-drive, so `getcwd()` returns something like
`'A:\\DOS'`.

### `os.path.*` — path helpers

A minimal subset:

- `os.path.exists(p)` — bool
- `os.path.isfile(p)` / `os.path.isdir(p)`
- `os.path.join(*parts)` — joins with `\` on DOS
- `os.path.split(p)` — `(head, tail)`
- `os.path.dirname(p)` / `os.path.basename(p)`

```python
>>> os.path.join('C:\\DOS', 'AUTOEXEC.BAT')
'C:\\DOS\\AUTOEXEC.BAT'
>>> os.path.basename(_)
'AUTOEXEC.BAT'
```

## Environment

### `os.getenv(name, default=None)`

Read a DOS environment variable (set with `SET name=value` in
COMMAND.COM):

```python
>>> os.getenv('PATH')
'C:\\DOS;C:\\WINDOWS;...'
>>> os.getenv('NONESUCH', '<unset>')
'<unset>'
```

### `os.environ` — full env as a `dict`

⚠️ Mutating `os.environ` does NOT propagate to the parent shell
when MP exits. DOS doesn't have a `putenv` syscall that affects
the parent; the change is purely in-process.

## Misc

### `os.urandom(n)`

Returns `n` bytes of random data. On this port, seeded from the
BIOS tick counter at startup; sufficient for non-crypto uses.
**Do not** use as a CSPRNG.

### `os.uname()`

Returns a `(sysname, nodename, release, version, machine)` named
tuple:

```python
>>> os.uname()
(sysname='fdmp', nodename='fdmp', release='1.26.0',
 version='MicroPython v1.26.0 on 2026-05-01', machine='uc386-dos with i386')
```

## File-descriptor I/O

The lower-level file API (`os.open`, `os.read`, `os.write`,
`os.close`, `os.lseek`) is available — `open()` actually delegates
to it. Use it when you need INT 21h-flavoured semantics directly.

```python
fd = os.open('FILE.TXT', os.O_RDONLY)
data = os.read(fd, 1024)
os.close(fd)
```

Open flags: `os.O_RDONLY`, `os.O_WRONLY`, `os.O_RDWR`.

## DOS-specific helpers

For raw INT 21h dispatch, see [`dosint21`](dosint21.md). For
direct memory access, see [`machine`](machine.md).

---

*Credit: surface adapted from
[MicroPython os docs](https://docs.micropython.org/en/latest/library/os.html)
(MIT, © 2014-2024 Damien P. George and contributors). DOS notes
are this port's.*
