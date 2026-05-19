---
title: select
---

# `select` — I/O multiplexing

```python
import select
```

Wait for one or more file descriptors (sockets, mostly) to become
ready for read / write / error. Backed by lwIP's `select()` for
sockets.

## `select.poll()` — preferred API

Returns a poll object.

```python
poller = select.poll()
poller.register(sock, select.POLLIN)        # wait for readable
poller.register(sock2, select.POLLIN | select.POLLOUT)

events = poller.poll(timeout_ms=100)
for obj, ev in events:
    if ev & select.POLLIN:
        data = obj.recv(1024)
        ...
    if ev & select.POLLOUT:
        ...

poller.unregister(sock)
```

Event flags:

- `select.POLLIN` — readable
- `select.POLLOUT` — writable
- `select.POLLERR` — error
- `select.POLLHUP` — connection hung up
- `select.POLLNVAL` — invalid fd

`timeout_ms` of `None` blocks indefinitely; `0` polls without
waiting; positive integer blocks up to that many milliseconds.

## `select.select(rl, wl, xl, timeout=None)` — BSD-style

```python
ready_r, ready_w, ready_x = select.select([sock], [], [], 1.0)
if ready_r:
    data = sock.recv(1024)
```

`timeout` is in seconds (float). `None` blocks.

## Example: echo server

```python
import socket, select

srv = socket.socket()
srv.bind(('0.0.0.0', 1234))
srv.listen(5)
poller = select.poll()
poller.register(srv, select.POLLIN)
clients = {}     # fd → socket

while True:
    for fd, ev in poller.poll(1000):
        if fd is srv:
            conn, addr = srv.accept()
            poller.register(conn, select.POLLIN)
            clients[id(conn)] = conn
        elif ev & select.POLLIN:
            data = fd.recv(1024)
            if data:
                fd.send(data)
            else:
                poller.unregister(fd)
                fd.close()
                clients.pop(id(fd), None)
```

## DOS specifics

- Only socket fds work with `select` — file fds will silently
  always-report-ready.
- The lwIP timer must be polled regularly (`lwip.callback()`) for
  data to actually arrive at sockets. Most users handle this by
  setting short timeouts on `poll()`/`select()` and calling
  `lwip.callback()` in the loop.

---

*Credit:
[MicroPython select docs](https://docs.micropython.org/en/latest/library/select.html)
(MIT).*
