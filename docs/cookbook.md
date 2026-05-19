---
title: Cookbook
---

# Cookbook

Ten-minute recipes for common tasks.

## Read a text file line by line

```python
with open('CONFIG.SYS', 'r') as f:
    for line in f:
        line = line.rstrip('\r\n')
        if line and not line.startswith(';'):
            print(line)
```

## Write a text file

```python
with open('OUT.TXT', 'w') as f:
    f.write('hello\r\n')
    f.write('world\r\n')
```

(Yes — write `\r\n` explicitly. There's no universal newline
translation on this port.)

## Hash a download to verify

```python
import hashlib

want = 'b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9'
h = hashlib.sha256()
with open('DOWNLOAD.BIN', 'rb') as f:
    while True:
        chunk = f.read(4096)
        if not chunk: break
        h.update(chunk)
got = h.hexdigest()
if got != want:
    raise SystemExit('hash mismatch: got ' + got)
print('ok')
```

## Walk a directory tree

```python
import os

def walk(top):
    for name in os.listdir(top):
        if name in ('.', '..'):
            continue
        path = top + '\\' + name if top else name
        st = os.stat(path)
        if st[0] & 0x4000:           # S_IFDIR
            yield path + '\\'
            yield from walk(path)
        else:
            yield path

for p in walk('C:\\DOS'):
    print(p)
```

## Set up the network (static)

```python
import lwip, uc386_net, time

lwip.reset()
if uc386_net.eth_init(False) != 0:
    raise RuntimeError('no packet driver')
uc386_net.eth_set_static('10.0.2.15', '255.255.255.0', '10.0.2.2')
for _ in range(5):
    lwip.callback()
    time.sleep_ms(50)
print('ifup:', uc386_net.eth_status())
```

## Set up the network (DHCP)

```python
import lwip, uc386_net, time

lwip.reset()
uc386_net.eth_init(False)
uc386_net.eth_set_dhcp()

for _ in range(50):
    lwip.callback()
    time.sleep_ms(100)
    if uc386_net.eth_status()[3]:
        break
else:
    raise SystemExit('DHCP timed out')

print('got:', uc386_net.eth_status())
```

## Connect a TCP socket

```python
import socket

addr = socket.getaddrinfo('10.0.2.2', 1234)[0][-1]
s = socket.socket()
s.settimeout(5.0)
s.connect(addr)
s.send(b'GET / HTTP/1.0\r\n\r\n')
resp = b''
while True:
    chunk = s.recv(1024)
    if not chunk: break
    resp += chunk
s.close()
print(resp.decode('cp437'))
```

## Run a remote command via SSH

```python
import socket, _ssh

s = socket.socket()
s.settimeout(0.1)
s.connect(socket.getaddrinfo('10.0.2.2', 22)[0][-1])
sess = _ssh.Session(s)
sess.userauth_password('user', 'pass')
out = sess.exec('uname -a')
print(out.decode())
sess.close()
```

## Talk to an HTTPS API

```python
import socket, ssl, json

s = socket.socket()
s.connect(socket.getaddrinfo('api.example.com', 443)[0][-1])
ss = ssl.wrap_socket(s, server_hostname='api.example.com')
ss.write(b'GET /status HTTP/1.0\r\nHost: api.example.com\r\n\r\n')
resp = b''
while True:
    chunk = ss.read(1024)
    if not chunk: break
    resp += chunk
ss.close()

# Parse the response
header, _, body = resp.partition(b'\r\n\r\n')
data = json.loads(body)
print(data['status'])
```

## A poll-every-N-seconds loop

```python
import time

PERIOD_MS = 5000
next_t = time.ticks_add(time.ticks_ms(), PERIOD_MS)

while True:
    if time.ticks_diff(next_t, time.ticks_ms()) <= 0:
        do_one_iteration()
        next_t = time.ticks_add(next_t, PERIOD_MS)
    time.sleep_ms(10)
```

## Cancellable wait with Ctrl-C

```python
import time

try:
    while True:
        do_something()
        time.sleep(0.5)
except KeyboardInterrupt:
    print('canceled by user')
```

## Compute a CRC32 in chunks

```python
import binascii

crc = 0
with open('FILE.BIN', 'rb') as f:
    while True:
        chunk = f.read(4096)
        if not chunk: break
        crc = binascii.crc32(chunk, crc)
print(f'crc32 = {crc:08x}')
```

## Decompress a .gz file

```python
import deflate

with open('PAYLOAD.GZ', 'rb') as src, \
     open('PAYLOAD.TXT', 'wb') as dst:
    d = deflate.DeflateIO(src, deflate.GZIP)
    while True:
        chunk = d.read(4096)
        if not chunk: break
        dst.write(chunk)
```

## Run an external `.py` file

```python
# As a module (top-level statements run once, imports cached)
import myscript

# Re-running:
import sys
del sys.modules['myscript']
import myscript

# Or by exec'ing the source — globals merge into yours:
with open('SCRIPT.PY') as f:
    exec(f.read())
```

## Cooperative concurrency with asyncio

```python
import asyncio

async def slow():
    await asyncio.sleep(2)
    return 'slow done'

async def fast():
    await asyncio.sleep(1)
    return 'fast done'

async def main():
    results = await asyncio.gather(slow(), fast())
    print(results)

asyncio.run(main())
```
