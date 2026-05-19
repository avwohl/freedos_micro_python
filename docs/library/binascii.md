---
title: binascii
---

# `binascii` — binary ↔ ASCII conversions

```python
import binascii
```

Hex, base64, CRC32. Useful for encoding binary blobs to put in
config files, hashing for cache keys, or sanity-checking
downloads.

## Hex

### `binascii.hexlify(data, sep=None)` → `bytes`

```python
binascii.hexlify(b'\x00\x01\xff')          # b'0001ff'
binascii.hexlify(b'\x00\x01\xff', '-')     # b'00-01-ff'
```

### `binascii.unhexlify(s)` → `bytes`

```python
binascii.unhexlify(b'0001ff')              # b'\x00\x01\xff'
```

### Methods on bytes themselves

`bytes.hex()` and `bytes.fromhex()` do the same thing:

```python
b'\x00\x01\xff'.hex()                      # '0001ff'
bytes.fromhex('0001ff')                    # b'\x00\x01\xff'
```

## Base64

### `binascii.b2a_base64(data, *, newline=True)` → `bytes`

```python
binascii.b2a_base64(b'hello')              # b'aGVsbG8=\n'
binascii.b2a_base64(b'hello', newline=False)  # b'aGVsbG8='
```

### `binascii.a2b_base64(s)` → `bytes`

```python
binascii.a2b_base64(b'aGVsbG8=')           # b'hello'
```

## CRC32

### `binascii.crc32(data, init=0)` → `int`

Standard CRC-32 (ISO 3309 polynomial, same one ZIP uses):

```python
binascii.crc32(b'hello')                   # 907060870
hex(binascii.crc32(b'hello'))              # '0x3610a686'
```

Chunked CRC — pass the previous result as `init`:

```python
crc = 0
with open('FILE.TXT', 'rb') as f:
    while True:
        chunk = f.read(4096)
        if not chunk: break
        crc = binascii.crc32(chunk, crc)
print('crc:', hex(crc))
```

## Example: hex-dump

```python
def hexdump(data, width=16):
    for i in range(0, len(data), width):
        chunk = data[i:i+width]
        h = binascii.hexlify(chunk, ' ').decode()
        t = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        print(f'{i:08x}  {h:<48}  {t}')

with open('MP.EXE', 'rb') as f:
    hexdump(f.read(64))
```

---

*Credit:
[MicroPython binascii docs](https://docs.micropython.org/en/latest/library/binascii.html)
(MIT).*
