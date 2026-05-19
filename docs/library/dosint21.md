---
title: dosint21
---

# `dosint21` — raw INT 21h dispatch

```python
import dosint21
```

A port-specific module that exposes the lower-level
`dos_int21_open` / `_read` / `_write` / `_close` / `_lseek`
primitives directly. Useful when you need DOS-flavoured semantics
(get-error-code, specific access modes) that `open()` doesn't
expose.

⚠️ The Python `open()` builtin already uses this path. Only reach
for the raw API when you need something `open()` can't give you.

## Functions

### `dosint21.open(path, mode)` → fd

`mode` is the DOS AH=3D access-mode field:

- `0` — read
- `1` — write
- `2` — read-write

Returns the DOS file handle on success (0–255). Raises `OSError`
on failure.

```python
import dosint21
fd = dosint21.open('CONFIG.SYS', 0)
print('opened fd', fd)
```

### `dosint21.read(fd, n)` → bytes

```python
data = dosint21.read(fd, 1024)
print(data.decode('cp437'))    # DOS default codepage
```

### `dosint21.write(fd, buf)` → int

```python
n = dosint21.write(fd, b'hello\r\n')
```

### `dosint21.close(fd)`

```python
dosint21.close(fd)
```

### `dosint21.lseek(fd, offset, whence=0)` → int

`whence`: 0 = SEEK_SET, 1 = SEEK_CUR, 2 = SEEK_END.

```python
dosint21.lseek(fd, 0, 2)     # to end
size = dosint21.lseek(fd, 0, 1)    # current position = size
```

### `dosint21.fsize(fd)` → int

Convenience: returns file size in bytes without disturbing the
current seek position.

## How it works under the hood

All calls go through:

```
python → C dos_int21_open(...)
       → dos_int21_call(rmcs)
       → pktdrv_int_invoke(0x31, dpmi)   ; DPMI fn 0x0301
       → INT 0x31 (PM)
       → DPMI host's int 31 handler
       → switches to real mode at our CD 21 CB thunk
       → real INT 0x21
       → returns through DPMI to PM caller
```

The thunk + bounce buffer are pre-allocated at `main()` entry
while the C stack is shallow, because PMODE/W's DPMI dispatch has
deep-stack sensitivities.

## See also

- [`os`](os.md) — high-level filesystem
- [`machine`](machine.md) — direct memory poke
- The C source: `port/dosint21_uc386dos.c` in the repo

---

*This is a port-specific module; no upstream-MicroPython
counterpart.*
