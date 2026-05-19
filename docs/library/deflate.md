---
title: deflate
---

# `deflate` — DEFLATE / zlib / gzip

```python
import deflate
```

Decompressor (uzlib) and compressor (DEFLATE_COMPRESS). Useful for
unpacking .gz files, parsing .zip archives, talking to HTTP servers
that send Content-Encoding: gzip.

## Wrappers

The `deflate.DeflateIO(stream, format=AUTO, wbits=0, close=False)`
class wraps an underlying byte-stream and gives you a file-like
object that transparently decompresses on read or compresses on
write.

| Format constant      | What it is                          |
|----------------------|-------------------------------------|
| `deflate.AUTO`       | Auto-detect on read (zlib or gzip)  |
| `deflate.RAW`        | Raw DEFLATE blocks, no header       |
| `deflate.ZLIB`       | zlib (DEFLATE + 2-byte header + adler32) |
| `deflate.GZIP`       | gzip (DEFLATE + 10-byte header + crc32) |

## Decompress

```python
import deflate, io

# Decompress an in-memory gzip blob
compressed = b'\x1f\x8b\x08\x00...'      # gzip magic
with deflate.DeflateIO(io.BytesIO(compressed), deflate.GZIP) as d:
    plain = d.read()
print(plain)
```

Or stream-decompress from a file:

```python
with open('PAYLOAD.GZ', 'rb') as f:
    d = deflate.DeflateIO(f, deflate.GZIP)
    out = open('PAYLOAD.TXT', 'wb')
    while True:
        chunk = d.read(4096)
        if not chunk: break
        out.write(chunk)
    out.close()
```

## Compress

Wrap a writable stream:

```python
import deflate, io

buf = io.BytesIO()
with deflate.DeflateIO(buf, deflate.GZIP) as z:
    z.write(b'hello world\n' * 100)

compressed = buf.getvalue()
print(len(compressed), 'bytes after gzip')
```

## Sanity test

```python
import deflate, io

src = b'lorem ipsum dolor sit amet ' * 50
buf = io.BytesIO()
with deflate.DeflateIO(buf, deflate.RAW) as z:
    z.write(src)

# Round-trip
buf.seek(0)
with deflate.DeflateIO(buf, deflate.RAW) as d:
    out = d.read()

assert out == src
print('compressed', len(src), '->', len(buf.getvalue()), 'bytes')
```

---

*Credit:
[MicroPython deflate docs](https://docs.micropython.org/en/latest/library/deflate.html)
(MIT).*
