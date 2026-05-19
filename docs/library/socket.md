---
title: socket
---

# `socket` — BSD sockets over lwIP

```python
import socket
```

The standard BSD socket API — TCP, UDP, DNS. Backed by lwIP
(`lwip` module), which in turn talks to the DOS packet driver at
INT 60h (or dosiz's emulated one).

**Before using sockets**, the network stack must be up:

```python
import lwip, uc386_net
lwip.reset()
uc386_net.eth_init(False)          # detect packet driver
uc386_net.eth_set_static('10.0.2.15', '255.255.255.0', '10.0.2.2')
# … or eth_set_dhcp() and wait for lease
```

## Constants

Address families: `socket.AF_INET` (IPv4 only on this port).

Socket types: `socket.SOCK_STREAM` (TCP), `socket.SOCK_DGRAM` (UDP).

Common setsockopt levels/names: `socket.SOL_SOCKET`,
`socket.SO_REUSEADDR`, `socket.TCP_NODELAY`. (See "Quirks" below
for a `TCP_NODELAY` gotcha.)

## Constructor

### `socket.socket(family=AF_INET, type=SOCK_STREAM, proto=0)`

```python
s = socket.socket()                                     # TCP
u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)    # UDP
```

## Client TCP

```python
s = socket.socket()
addr = socket.getaddrinfo('10.0.2.2', 1234)[0][-1]
s.connect(addr)
s.send(b'hello\n')
data = s.recv(1024)
s.close()
```

### `getaddrinfo(host, port)`

Returns a list of `(family, type, proto, canonname, sockaddr)`
5-tuples. Use the last element as the `sockaddr` for `connect`,
`bind`, etc.:

```python
addr_info = socket.getaddrinfo('example.com', 80)
addr = addr_info[0][-1]    # ('93.184.216.34', 80)
s.connect(addr)
```

DNS resolution goes through lwIP — set a nameserver via
`uc386_net.eth_set_static('myip', 'mask', 'gw', 'dns')` if your
DHCP didn't.

## Server TCP

```python
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('0.0.0.0', 1234))
s.listen(5)

while True:
    conn, addr = s.accept()
    print('got', addr)
    data = conn.recv(1024)
    conn.send(b'OK\n')
    conn.close()
```

`accept()` blocks until a client connects.

## UDP

```python
u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u.bind(('0.0.0.0', 0))                        # any free port

u.sendto(b'ping', ('10.0.2.2', 9999))
data, addr = u.recvfrom(1500)
print('from', addr, ':', data)
```

## Socket methods

| Method                       | Purpose                              |
|------------------------------|--------------------------------------|
| `.connect(addr)`             | TCP connect                          |
| `.bind(addr)`                | bind to local address                |
| `.listen(backlog)`           | listening socket                     |
| `.accept()`                  | accept connection — `(conn, addr)`   |
| `.send(buf)` / `.sendall(buf)` | TCP send                          |
| `.recv(n)`                   | TCP read up to n bytes               |
| `.recv_into(buf, n=-1)`      | read into pre-allocated buffer       |
| `.sendto(buf, addr)`         | UDP send                             |
| `.recvfrom(n)`               | UDP read — `(data, addr)`            |
| `.close()`                   | close                                |
| `.settimeout(secs)`          | float, or `None` for blocking        |
| `.setblocking(bool)`         | True = blocking, False = nonblocking |
| `.setsockopt(level, name, val)` | set option                        |
| `.fileno()`                  | underlying fd (for `select`)         |
| `.makefile('rwb')`           | file-like wrapper                    |

## Timeouts

```python
s.settimeout(0.1)            # 100 ms; raises OSError(ETIMEDOUT) on expiry
try:
    data = s.recv(1024)
except OSError as e:
    if e.args[0] != errno.ETIMEDOUT:
        raise
    print('no data within 100 ms')
```

`settimeout(None)` (the default) is blocking forever.

## Quirks on this port

- **TCP_NODELAY constant** in upstream MicroPython matches lwIP's
  internal `TF_NODELAY = 0x40`, not the POSIX `TCP_NODELAY = 1`.
  In practice nobody hits this — the network code works without
  setting `NODELAY` after the EOF→EAGAIN fix in `port/modssh_*.c`
  for our SSH stack. If you need to set it, use `0x40`.
- **`socket.recv()` returns `b''` on clean peer FIN**, like POSIX.
  Our SSH layer maps `recv() == 0` to `-EAGAIN` so libssh2's
  transport doesn't bail. If you're writing your own protocol on
  top of `socket`, treat `b''` from `recv()` as "remote closed".
- **lwIP poll**. The lwIP timers + packet queue need
  `lwip.callback()` calls. The high-level helpers (asyncio
  streams, the SSH module) call it automatically; raw `socket`
  code in a tight loop without sleeps may need to call it
  manually.

## See also

- [`ssl`](ssl.md) — wrap a socket in TLS
- [`_ssh`](_ssh.md) — wrap a socket in SSH
- [`select`](select.md) — multiplex multiple sockets
- [`lwip`](lwip.md) — the underlying stack
- [`uc386_net`](uc386_net.md) — interface configuration

---

*Credit: shape from
[MicroPython socket docs](https://docs.micropython.org/en/latest/library/socket.html)
(MIT, © 2014-2024 Damien P. George and contributors). DOS-specific
notes are this port's.*
