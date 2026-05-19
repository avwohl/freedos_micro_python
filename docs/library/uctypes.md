---
title: uctypes
---

# `uctypes` — typed memory access

```python
import uctypes
```

Define C-like struct layouts on top of `bytes` / `bytearray` /
`memoryview`. Useful for parsing on-disk DOS structures (FAT
boot sectors, MZ EXE headers, etc.) without writing pack/unpack
formats by hand.

## Concepts

A *struct definition* is a Python dict describing field
names → offset-and-type pairs. Pass it + a byte buffer + an endian
to `uctypes.struct()` and you get back an object whose attributes
read/write through to the buffer.

## Field type encodings

The type encoding combines the size class with the offset.

| Encoding              | Width |
|-----------------------|-------|
| `uctypes.UINT8`       | 1 byte |
| `uctypes.INT8`        | 1 byte (signed) |
| `uctypes.UINT16`      | 2 bytes |
| `uctypes.INT16`       | 2 bytes (signed) |
| `uctypes.UINT32`      | 4 bytes |
| `uctypes.INT32`       | 4 bytes (signed) |
| `uctypes.UINT64`      | 8 bytes |
| `uctypes.INT64`       | 8 bytes (signed) |
| `uctypes.FLOAT32`     | 4 bytes |
| `uctypes.FLOAT64`     | 8 bytes |
| `uctypes.ARRAY` + `n` | array of n elems |
| `uctypes.PTR`         | pointer |

Endian: `uctypes.LITTLE_ENDIAN`, `uctypes.BIG_ENDIAN`,
`uctypes.NATIVE`.

## Example: MZ EXE header

```python
import uctypes

MZ_DESC = {
    'magic':           0  | uctypes.ARRAY | (2 << uctypes.ARRAY_SIZE_BITS) | uctypes.UINT8,
    'bytes_in_last':   2  | uctypes.UINT16,
    'pages':           4  | uctypes.UINT16,
    'reloc_count':     6  | uctypes.UINT16,
    'header_paras':    8  | uctypes.UINT16,
    'min_alloc':      10  | uctypes.UINT16,
    'max_alloc':      12  | uctypes.UINT16,
    'init_ss':        14  | uctypes.UINT16,
    'init_sp':        16  | uctypes.UINT16,
    'checksum':       18  | uctypes.UINT16,
    'init_ip':        20  | uctypes.UINT16,
    'init_cs':        22  | uctypes.UINT16,
    'reloc_offset':   24  | uctypes.UINT16,
    'overlay':        26  | uctypes.UINT16,
}

with open('MP.EXE', 'rb') as f:
    buf = f.read(28)

hdr = uctypes.struct(uctypes.addressof(buf), MZ_DESC, uctypes.LITTLE_ENDIAN)
print('magic:', bytes(hdr.magic))     # b'MZ'
print('pages:', hdr.pages)
print('CS:IP at start:', hex(hdr.init_cs), hex(hdr.init_ip))
```

The struct shape uses MicroPython's compact representation —
each field is `offset | type`. For nested structures and arrays,
the upstream
[uctypes guide](https://docs.micropython.org/en/latest/library/uctypes.html)
has the full layout grammar.

## Use cases on DOS

- Parse DOS file headers (.EXE, .COM, .OBJ)
- Walk FAT boot sectors (BPB)
- Read INT 0x21 results that come back as structs in the DTA
- Interpret packet driver responses (Crynwr access_type, etc.)

For one-off parsing the `struct` module is usually simpler;
`uctypes` shines when the same struct definition is reused many
times against in-place memory.

---

*Credit:
[MicroPython uctypes docs](https://docs.micropython.org/en/latest/library/uctypes.html)
(MIT).*
