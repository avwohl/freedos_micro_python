---
title: Built-in types
---

# Built-in types

The complete set of built-in types you can construct without
importing anything. For each, the canonical operations and the
DOS-relevant gotchas.

## `int` — arbitrary-precision integer

```python
n = 42                # int literal
n = 0xFF              # hex
n = 0b1010            # binary
n = 0o755             # octal
n = 1_000_000         # underscore separator (parsed away)
```

Heap-allocated when the value exceeds the small-int range
(`-2³⁰ .. 2³⁰-1` ish — i386 small-int). Otherwise lives in the
object pointer itself.

Operations: `+`, `-`, `*`, `/`, `//`, `%`, `**`, `<<`, `>>`,
`&`, `|`, `^`, `~`, comparison, `abs()`, `divmod()`, `pow(b, e, m)`.

Methods:

- `.bit_length()` — number of bits needed to represent value
- `.to_bytes(length, byteorder='big', *, signed=False)`
- `int.from_bytes(b, byteorder='big', *, signed=False)`

```python
(255).bit_length()                     # 8
(256).to_bytes(2, 'big')               # b'\x01\x00'
int.from_bytes(b'\xff\xff', 'big')     # 65535
```

The whole 64-bit divmod path goes through uc386's `int 0x80` trap;
your `7 // 3` is compiled to a real division instruction, but the
formatter `'{:d}'.format(2**40)` uses the int 80 path. This works
under qemu+FreeDOS, dosiz, and DOSBox-X.

## `float` — double-precision

x87 FPU, 64-bit IEEE 754. Range ~1.8e308.

```python
3.14
1e6                   # 1000000.0
.5                    # 0.5
2.0 ** 53             # 9007199254740992.0
float('inf'), float('-inf'), float('nan')
```

Math functions: see [`math`](library/math.md).

Formatting uses the EXACT formatter (shortest round-trippable
representation), so `print(0.1 + 0.2)` shows `0.30000000000000004`
(the actual stored value) instead of `0.3` like APPROX would.

## `bool` — `True` / `False`

Subclass of `int`. `True == 1`, `False == 0`. `bool(x)` follows
Python truthiness: zero numbers / empty strings / empty
collections / `None` are False; everything else is True.

## `str` — unicode string

UTF-8 encoded internally; indexing is by character (not byte).

```python
s = 'hello'
s[0]                  # 'h'
s[-1]                 # 'o'
len(s)                # 5
s.upper()             # 'HELLO'
'  pad  '.strip()     # 'pad'
'a,b,c'.split(',')    # ['a', 'b', 'c']
','.join(['a','b'])   # 'a,b'
'hello'.find('ll')    # 2
'hello'.replace('l', 'L')  # 'heLLo'
'  pad'.lstrip()      # 'pad'
'abc'.startswith('ab') # True
'abc'.endswith('bc')  # True
'42'.isdigit()        # True
'HELLO'.lower()       # 'hello'
'hello world'.title() # 'Hello World'
'hi'.center(6, '.')   # '..hi..'
'a\nb\nc'.splitlines()  # ['a', 'b', 'c']
'a;b'.partition(';')  # ('a', ';', 'b')
'a'.encode()          # b'a'
```

### Formatting

```python
'{} = {}'.format('x', 42)       # 'x = 42'
'{:>5}'.format('hi')            # '   hi'
'{:.2f}'.format(3.14159)        # '3.14'
'{:08x}'.format(255)            # '000000ff'
f'{name!r:>10}'                 # f-string, right-aligned 10-wide repr
f'{x=}'                         # f-string =-form: "x=...value..."
```

## `bytes` / `bytearray` — 8-bit binary

`bytes` is immutable; `bytearray` is mutable.

```python
b = b'\x00\x01\xff'
b[0]                  # 0  (int, not str)
b[1:3]                # b'\x01\xff'
len(b)                # 3
b + b'\xab'           # b'\x00\x01\xff\xab'
b.hex()               # '0001ff'
bytes.fromhex('00 01 ff')  # b'\x00\x01\xff'

ba = bytearray(b'abc')
ba[0] = ord('A')      # bytearray(b'Abc')
ba.extend(b'!')       # bytearray(b'Abc!')
```

`bytes` has all the search/split/replace methods that `str` has,
just operating on byte values.

`b'...'.decode('utf-8')` → `str`; `'...'.encode('utf-8')` →
`bytes`. Encoding defaults to UTF-8.

## `list` — mutable sequence

```python
xs = [1, 2, 3]
xs.append(4)               # [1, 2, 3, 4]
xs.extend([5, 6])          # [1, 2, 3, 4, 5, 6]
xs.insert(0, 0)            # [0, 1, 2, 3, 4, 5, 6]
xs.remove(3)               # [0, 1, 2, 4, 5, 6]  (first occurrence)
xs.pop()                   # 6; xs == [0, 1, 2, 4, 5]
xs.pop(0)                  # 0
xs.index(4)                # 2
xs.count(2)                # 1
xs.sort()
xs.sort(key=abs, reverse=True)
xs.reverse()
xs.clear()
xs.copy()                  # shallow copy

# Slicing
xs[1:3]                    # subsequence
xs[::-1]                   # reversed copy
xs[::2]                    # every other element
xs[1:3] = [10, 20, 30]     # slice assignment

# Comprehension
ys = [x*x for x in range(5)]   # [0, 1, 4, 9, 16]
```

## `tuple` — immutable sequence

```python
pt = (3, 4)
x, y = pt                  # unpacking
single = (1,)              # trailing comma needed for 1-tuple
empty = ()
pt[0]                      # 3
pt + (5, 6)                # (3, 4, 5, 6)
```

Tuples are hashable (if their elements are), so you can use them
as dict keys or set members.

## `dict` — key/value map, insertion-ordered

```python
d = {'a': 1, 'b': 2}
d['c'] = 3                 # insert/update
d.get('z', 0)              # 0 (default)
d.setdefault('z', 99)      # 99 (and now d['z'] == 99)
d.pop('a')                 # removes + returns
d.popitem()                # removes + returns last inserted
'a' in d                   # membership tests against keys
list(d.keys())             # ['b', 'c', 'z']
list(d.values())
list(d.items())
for k, v in d.items(): ...
d.update({'x': 10, 'y': 20})

# Comprehension
{x: x*x for x in range(5)}
```

`dict.fromkeys(iterable, value=None)` constructs a new dict where
each key maps to the same value.

## `set` / `frozenset` — unordered unique collection

```python
s = {1, 2, 3}
s.add(4)
s.remove(2)                # KeyError if missing
s.discard(99)              # OK if missing
s | {3, 4, 5}              # union → {1, 3, 4, 5}
s & {3, 4, 5}              # intersection → {3, 4}
s - {3}                    # difference
s ^ {3, 99}                # symmetric diff
3 in s                     # True

frozenset([1, 2, 3])       # immutable, hashable
```

## `range(stop)` / `range(start, stop, step)`

Lazy integer sequence — no list materialized. Useful in `for` loops
and comprehensions:

```python
range(5)                      # range(5)
list(range(5))                # [0, 1, 2, 3, 4]
list(range(10, 0, -2))        # [10, 8, 6, 4, 2]
```

## `memoryview(obj)` — zero-copy view

```python
buf = bytearray(b'\x00' * 16)
mv = memoryview(buf)
mv[4:8] = b'\xAA\xAA\xAA\xAA'   # writes through into buf, no copy
```

Useful for chunked I/O without allocation.

## `NoneType` — `None`

Singleton sentinel. Used as the default return when a function
doesn't `return` anything.

```python
def f(): pass
f() is None             # True
```

---

*Credit: type summaries adapted from the
[Python data-model docs](https://docs.python.org/3/reference/datamodel.html)
(PSF License, © 2001-2024 Python Software Foundation).*
