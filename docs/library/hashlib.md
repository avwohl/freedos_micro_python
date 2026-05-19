---
title: hashlib
---

# `hashlib` — cryptographic hashes

```python
import hashlib
```

This port ships SHA-256, SHA-1, and MD5, all backed by the same
axtls implementations used by `ssl` and `_ssh`. Real
implementations — not stubs.

## Common API

All three hash objects follow the same shape:

```python
h = hashlib.sha256()        # constructor (also: .sha1(), .md5())
h.update(b'hello')          # absorb more bytes
h.update(b' world')
h.digest()                  # b'<raw 32 bytes for sha256>'
h.hexdigest()               # '<lowercase hex string>'
```

Optionally pass initial data:

```python
h = hashlib.sha256(b'hello world')
h.hexdigest()
# 'b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9'
```

## Examples

### Hash a file in chunks

```python
def file_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            chunk = f.read(4096)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()

print(file_sha256('MP.EXE'))
```

### HMAC (no `hmac` module, but you can DIY)

```python
def hmac_sha256(key, msg):
    block_size = 64
    if len(key) > block_size:
        key = hashlib.sha256(key).digest()
    key = key + b'\x00' * (block_size - len(key))
    o_pad = bytes(b ^ 0x5C for b in key)
    i_pad = bytes(b ^ 0x36 for b in key)
    inner = hashlib.sha256(i_pad + msg).digest()
    return hashlib.sha256(o_pad + inner).hexdigest()

print(hmac_sha256(b'secret', b'message'))
```

(The lower-level `_libssh2_axtls_hmac` API used internally by SSL/SSH
already does this; the Python `hmac` module isn't bundled, but the
above 8-liner is enough.)

## NOT included

These upstream Python algorithms aren't built into this port:

- `sha224`, `sha384`, `sha512` — backend supports them but not
  enabled by default
- `sha3_*` — not bundled
- `blake2b`, `blake2s` — not bundled

If you need them, the axtls SHA-384/512 implementations are
already linked in (used by SSH); enabling the Python wrapper is a
small port change.

---

*Credit: api shape from
[MicroPython hashlib docs](https://docs.micropython.org/en/latest/library/hashlib.html)
(MIT).*
