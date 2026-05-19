---
title: collections
---

# `collections` — high-level containers

```python
import collections
```

This port has two of the most useful: `deque` and `OrderedDict`.

## `collections.deque(iterable=(), maxlen=None)`

Double-ended queue. O(1) append/pop on either end. With
`maxlen=N`, becomes a bounded ring buffer (oldest entries fall
off when over capacity).

```python
from collections import deque

q = deque()
q.append(1)              # [1]
q.append(2)              # [1, 2]
q.appendleft(0)          # [0, 1, 2]
q.pop()                  # 2; q = [0, 1]
q.popleft()              # 0; q = [1]
len(q)                   # 1
1 in q                   # True
for x in q: ...
```

Bounded:

```python
log = deque(maxlen=100)            # last 100 entries
log.append('event 1')
log.append('event 2')
# After 100 appends, oldest start to drop.
```

Subscripting + iteration work; no slicing.

## `collections.OrderedDict([items])`

A `dict` that remembers insertion order — same as plain `dict` in
MicroPython (which is already insertion-ordered, like CPython 3.7+),
but `OrderedDict.move_to_end(key, last=True)` lets you reorder
explicitly:

```python
from collections import OrderedDict

cache = OrderedDict()
cache['a'] = 1
cache['b'] = 2
cache.move_to_end('a')      # now {'b': 2, 'a': 1}
cache.move_to_end('b', last=False)   # now {'b': 2, 'a': 1} — at start
cache.popitem(last=False)   # ('b', 2)  — LIFO/FIFO control
```

Useful for an LRU cache pattern:

```python
class LRU:
    def __init__(self, cap):
        self.cap = cap
        self.cache = OrderedDict()
    def get(self, k, default=None):
        if k in self.cache:
            self.cache.move_to_end(k)
            return self.cache[k]
        return default
    def put(self, k, v):
        self.cache[k] = v
        self.cache.move_to_end(k)
        if len(self.cache) > self.cap:
            self.cache.popitem(last=False)
```

## Not included

`namedtuple`, `Counter`, `defaultdict`, `ChainMap` are upstream
Python but not built into this port. `namedtuple` is easy to fake
with a small class; `Counter` is `dict.get(k, 0) + 1` away.

---

*Credit:
[MicroPython collections docs](https://docs.micropython.org/en/latest/library/collections.html)
(MIT).*
