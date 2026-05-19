---
title: struct
---

# `struct` — pack/unpack binary data

```python
import struct
```

Convert between Python values and C-style binary representations.
Useful for binary file formats, network protocols, and reading
DOS-era data structures.

## Format characters

| Code | C type             | Python | Size (bytes) |
|------|--------------------|--------|--------------|
| `b`  | signed char        | int    | 1            |
| `B`  | unsigned char      | int    | 1            |
| `h`  | short              | int    | 2            |
| `H`  | unsigned short     | int    | 2            |
| `i`  | int                | int    | 4            |
| `I`  | unsigned int       | int    | 4            |
| `l`  | long               | int    | 4            |
| `L`  | unsigned long      | int    | 4            |
| `q`  | long long          | int    | 8            |
| `Q`  | unsigned long long | int    | 8            |
| `f`  | float              | float  | 4            |
| `d`  | double             | float  | 8            |
| `s`  | char[N]            | bytes  | N            |
| `p`  | pascal string      | bytes  | N            |
| `x`  | pad byte           | —      | 1            |

Endianness / alignment prefix:

| Prefix | Byte order        | Size    |
|--------|-------------------|---------|
| `<`    | little-endian     | packed  |
| `>`    | big-endian        | packed  |
| `=`    | native            | packed  |
| `@`    | native            | aligned |
| `!`    | network (= `>`)   | packed  |

Default (no prefix) is native + aligned (`@`).

## Functions

### `struct.pack(fmt, *vals)` → `bytes`

```python
struct.pack('<I', 0xCAFEBABE)             # b'\xbe\xba\xfe\xca'
struct.pack('>HH', 1, 256)                # b'\x00\x01\x01\x00'
struct.pack('<BBBB', 1, 2, 3, 4)          # b'\x01\x02\x03\x04'
struct.pack('5s', b'hello')               # b'hello'
struct.pack('<If', 42, 3.14)              # 8 bytes
```

### `struct.unpack(fmt, buf)` → `tuple`

```python
struct.unpack('<I', b'\xbe\xba\xfe\xca')  # (3405691582,)
x, = struct.unpack('<I', buf)             # comma → unpack 1-tuple
```

### `struct.pack_into(fmt, buf, offset, *vals)` / `struct.unpack_from(fmt, buf, offset=0)`

Write/read in-place into a `bytearray` / `memoryview`:

```python
buf = bytearray(16)
struct.pack_into('<II', buf, 0, 0xDEADBEEF, 0xCAFEBABE)
struct.unpack_from('<II', buf, 0)         # (3735928559, 3405691582)
```

### `struct.calcsize(fmt)` → `int`

Size in bytes of one record of `fmt`:

```python
struct.calcsize('<I')                     # 4
struct.calcsize('<IIH')                   # 10
```

## Example: parse a BMP header

```python
import struct

with open('IMAGE.BMP', 'rb') as f:
    hdr = f.read(14)

magic, size, _, _, off = struct.unpack('<2sIHHI', hdr)
print('BMP magic:', magic, 'file size:', size, 'pixel offset:', off)
```

---

*Credit:
[MicroPython struct docs](https://docs.micropython.org/en/latest/library/struct.html)
(MIT).*
