---
title: The Python language
---

# The Python language, minimum viable summary

This page is a rapid-fire summary of the Python language as it runs
on this port. If you've used Python 3 elsewhere, it's mostly
"the same"; the gaps are noted at the end.

The canonical, full-length reference is the
[Python Language Reference](https://docs.python.org/3/reference/)
upstream — read it for the formal grammar. This page is the
"survival kit" version: enough to write working programs.

## Variables and types

No declaration needed; types are inferred:

```python
x = 42                    # int
y = 3.14                  # float
name = 'hello'            # str
data = b'\x00\x01\xff'    # bytes
flag = True               # bool (subclass of int)
nothing = None            # NoneType
```

The built-in types are: `int` (arbitrary precision), `float`
(double), `bool`, `str` (unicode), `bytes`, `bytearray`,
`memoryview`, `list`, `tuple`, `dict`, `set`, `frozenset`, `range`,
`NoneType`. See [Built-in types](types.md).

## Arithmetic

```python
2 + 3      # 5
2 - 3      # -1
2 * 3      # 6
7 / 2      # 3.5  (true division)
7 // 2     # 3    (floor division)
7 % 2      # 1    (modulo)
2 ** 10    # 1024 (power)
abs(-5)    # 5
divmod(7, 2)  # (3, 1)
```

Bitwise: `&`, `|`, `^`, `~`, `<<`, `>>`. Comparison: `==`, `!=`,
`<`, `<=`, `>`, `>=`. Boolean: `and`, `or`, `not`.

## Strings

```python
s = 'hello'
len(s)              # 5
s[0]                # 'h'
s[1:4]              # 'ell'
s[::-1]             # 'olleh'
s + ' world'        # 'hello world'
'no' * 3            # 'nonono'
'l' in s            # True

'hello'.upper()     # 'HELLO'
'  pad  '.strip()   # 'pad'
'a,b,c'.split(',')  # ['a', 'b', 'c']
','.join(['a','b']) # 'a,b'
'{} = {}'.format('x', 42)   # 'x = 42'
f'{name=}'                  # "name='hello'"  (f-strings work)
```

`bytes` is like `str` but holds 8-bit values and uses `b''`
literals: `b'\x00\xff'`. Convert with `s.encode()` /
`b.decode('utf-8')`.

## Containers

```python
# list — ordered, mutable
xs = [1, 2, 3]
xs.append(4)              # [1, 2, 3, 4]
xs[1:3]                   # [2, 3]
sum(xs)                   # 10

# tuple — ordered, immutable
pt = (3, 4)
x, y = pt                 # unpacking

# dict — key/value, insertion-ordered
d = {'a': 1, 'b': 2}
d['c'] = 3
d.get('z', 0)             # 0
'a' in d                  # True
for k, v in d.items(): ...

# set — unordered, unique
s = {1, 2, 3}
s.add(4)
s & {2, 4, 6}             # {2, 4}
```

## Control flow

```python
if x > 0:
    print('positive')
elif x < 0:
    print('negative')
else:
    print('zero')

while x > 0:
    x -= 1
else:
    print('drained')      # runs if while didn't break

for item in [1, 2, 3]:
    print(item)
else:
    print('done')         # runs if for didn't break

for i in range(5):        # 0,1,2,3,4
    pass
for i in range(2, 10, 3): # 2, 5, 8
    pass
```

`break` exits the loop; `continue` skips to the next iteration.
`pass` is a no-op (placeholder).

## Functions

```python
def greet(name, greeting='hello'):
    return f'{greeting}, {name}'

greet('World')                    # 'hello, World'
greet('World', greeting='hi')     # 'hi, World'

# *args / **kwargs
def collect(*args, **kw):
    return args, kw
collect(1, 2, a=3)                # ((1, 2), {'a': 3})

# Keyword-only args
def f(x, *, y=10):  ...
f(1, y=2)
f(1, 2)   # TypeError: f() takes 1 positional argument

# Lambda (anonymous function)
add = lambda a, b: a + b
add(2, 3)                         # 5
```

Functions are first-class values; you can pass them around, store
them in lists, return them from other functions, etc.

## Classes

```python
class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y
    def distance(self, other):
        dx = self.x - other.x
        dy = self.y - other.y
        return (dx * dx + dy * dy) ** 0.5
    def __repr__(self):
        return f'Point({self.x}, {self.y})'

p = Point(3, 4)
q = Point(0, 0)
p.distance(q)         # 5.0
```

Inheritance + `super()` work:

```python
class Named(Point):
    def __init__(self, x, y, name):
        super().__init__(x, y)
        self.name = name
```

Special methods (`__add__`, `__eq__`, `__hash__`, `__len__`,
`__iter__`, `__getitem__`, `__enter__` / `__exit__`, …) all work
as in CPython.

## Comprehensions and generators

```python
squares = [x * x for x in range(5)]              # [0, 1, 4, 9, 16]
even = [x for x in range(10) if x % 2 == 0]      # [0, 2, 4, 6, 8]
table = {x: x * x for x in range(5)}             # dict comp
unique = {x % 3 for x in range(10)}              # set comp

# Generator expression — lazy, no list built
total = sum(x * x for x in range(1000000))

# yield — turns a function into a generator
def counter(n):
    for i in range(n):
        yield i * i
list(counter(4))                                  # [0, 1, 4, 9]
```

## Exceptions

```python
try:
    open('NOPE.TXT')
except OSError as e:
    print('not found:', e)
except Exception as e:
    print('something else:', e)
else:
    print('open succeeded')
finally:
    print('always runs')

# raise
def withdraw(amt):
    if amt > balance:
        raise ValueError('overdraft')
```

Standard exceptions: `Exception`, `OSError`, `ValueError`,
`TypeError`, `KeyError`, `IndexError`, `AttributeError`,
`RuntimeError`, `StopIteration`, `KeyboardInterrupt`, …

## Context managers (`with`)

```python
with open('FILE.TXT') as f:
    data = f.read()
# f is closed here

# Reusable: any class with __enter__ + __exit__
class Timer:
    def __enter__(self):
        import time
        self.t0 = time.ticks_ms()
        return self
    def __exit__(self, *a):
        print('took', time.ticks_ms() - self.t0, 'ms')

with Timer():
    do_stuff()
```

## Modules

```python
import sys
from os import listdir, getcwd
import HELLO as hi      # rename on import
```

Any `.py` file in the current directory is importable by name (8.3
filename, case-insensitive on DOS). The bytecode is compiled on
import and cached in RAM (no `.pyc` files on disk — we don't have
upstream's mpy-tool plumbed in yet).

`sys.path` is `['']` by default — current directory only. Add to
it to import from elsewhere:

```python
import sys
sys.path.append('C:\\PYLIB')
import mylib    # looks in C:\PYLIB\MYLIB.PY
```

## Async (cooperative)

`async def` and `await` work; `asyncio` provides the event loop.
No threads — everything is cooperative.

```python
import asyncio

async def fetch(n):
    await asyncio.sleep(n)
    return n * 2

async def main():
    results = await asyncio.gather(fetch(1), fetch(2), fetch(3))
    print(results)            # [2, 4, 6]

asyncio.run(main())
```

See [asyncio](library/asyncio.md) for the full surface.

## Things that DON'T work (vs CPython)

- `_thread` — DOS is single-threaded.
- `cmath`, complex numbers — not built in (no use case yet).
- `weakref` — not built in (no use case yet).
- `__slots__` — class slot optimization is a no-op.
- `match` statement (PEP 634) — not enabled.
- `walrus` `:=` operator — enabled.
- `typing` module — not bundled. Type annotations are parsed and
  ignored.
- `socket.create_connection`, `urllib.*` — the high-level
  networking helpers are not bundled. Build them on top of
  `socket` if needed.
- `subprocess`, `multiprocessing`, `os.fork` — no process model on
  DOS.
- `pathlib` — use string paths.
- f-string equals-form `f'{x=}'` — works.

If a feature isn't listed above, try it; it probably works.

---

*Credit: language summary structure adapted from
[Python: Tutorial](https://docs.python.org/3/tutorial/)
(PSF License, © 2001-2024 Python Software Foundation). Examples
are original / DOS-adapted.*
