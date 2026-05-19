---
title: json
---

# `json` — JSON encoder / decoder

```python
import json
```

Convert between Python objects and JSON strings. Full RFC 8259.

## Decoding

### `json.loads(s)` — string → Python object

```python
json.loads('{"name": "Dave", "age": 42}')
# {'name': 'Dave', 'age': 42}

json.loads('[1, 2, 3]')          # [1, 2, 3]
json.loads('null')               # None
json.loads('true')               # True
json.loads('3.14')               # 3.14
```

### `json.load(file)` — file → Python object

```python
with open('CONFIG.JSO', 'r') as f:
    cfg = json.load(f)
```

⚠️ DOS uses 8.3 names. `.JSON` is 4-char extension and gets
truncated to `.JSO` on FAT. Save your JSON with `.JSO`, `.JSN`,
or `.CFG` to avoid surprises.

## Encoding

### `json.dumps(obj, separators=None)` — Python → string

```python
json.dumps({'name': 'Dave', 'age': 42})
# '{"name": "Dave", "age": 42}'

json.dumps([1, 2, 3])            # '[1, 2, 3]'
json.dumps([1, 2, 3], separators=(',', ':'))  # '[1,2,3]'  (no spaces)
```

### `json.dump(obj, file)` — Python → file

```python
with open('OUT.JSO', 'w') as f:
    json.dump({'ok': True}, f)
```

## Type mapping

| Python              | JSON           |
|---------------------|----------------|
| `dict`              | object         |
| `list`, `tuple`     | array          |
| `str`               | string         |
| `int`, `float`      | number         |
| `True` / `False`    | `true` / `false` |
| `None`              | `null`         |
| Anything else       | `TypeError`    |

Round-trip preserves types except `tuple` → `list`. No custom
encoders / decoders (no `default=` or `object_hook=` parameters).

## Example: config file pattern

```python
import json

DEFAULTS = {'ssh_user': 'root', 'ssh_port': 22, 'timeout': 30}

try:
    with open('CONFIG.JSO', 'r') as f:
        config = {**DEFAULTS, **json.load(f)}
except OSError:
    config = DEFAULTS.copy()

print('using user:', config['ssh_user'])
```

---

*Credit:
[MicroPython json docs](https://docs.micropython.org/en/latest/library/json.html)
(MIT).*
