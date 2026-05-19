---
title: WGET.PY
---

# `WGET.PY` — HTTPS streaming downloader

Standalone MicroPython script that ships with this port. Downloads
URLs over HTTP or HTTPS, streams to disk so the whole body never
sits in RAM. Follows up to 5 redirects.

Source: [`examples/wget.py`](https://github.com/avwohl/freedos_micro_python/blob/main/examples/wget.py).

## Usage

```
MP.EXE WGET.PY [-O outpath] [--ca-certs PATH] [-v] URL
```

(Strictly speaking: `MP.EXE` runs the REPL by default; to run a
script, pipe it into stdin: `MP.EXE < WGET.PY`. The script
internally parses its own args from `sys.argv`.)

### Examples

```
MP.EXE WGET.PY -O HELLO.HTM http://example.com/
MP.EXE WGET.PY -O CACERT.PEM https://curl.se/ca/cacert.pem --ca-certs CACERTS.PEM
MP.EXE WGET.PY -v https://example.com/
```

### Flags

- `-O OUTPATH` — write to OUTPATH; default `<basename of URL>` or
  `INDEX.HTM` if the URL has no path component.
- `--ca-certs PATH` — verify TLS certs against this PEM bundle.
  Without it, TLS is `CERT_NONE` (no verification).
- `-v` — verbose: log redirects, content-length, chunk progress.

### Network setup

`WGET.PY` assumes the network is up. Run this first:

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
```

Or use a static config. Or paste both into one big script that ends
with the actual `wget`.

### How it works

1. Parse URL → host, port, path
2. `socket.getaddrinfo(host, port)` (DNS via lwIP)
3. `socket.socket()` + `.connect()`
4. If HTTPS: wrap with `ssl.wrap_socket(server_hostname=...,
   cert_reqs=CERT_REQUIRED, ca_certs=PATH)` if `--ca-certs`
5. Write the GET request: `GET <path> HTTP/1.1\r\nHost: ...\r\n
   Connection: close\r\nUser-Agent: WGET.PY/0.1\r\n\r\n`
6. Read headers until `\r\n\r\n`; parse status + content-length +
   location
7. If 30x: redirect (max 5)
8. Open OUTPATH for write
9. Stream the body in 4 KB chunks until either content-length is
   met or the server closes

## Limits

- HTTPS does work but is RAM-heavy because of axtls cert
  verification — count on ~300 KB free heap at handshake time
- Only follows redirects from a 30x response with a `Location:`
  header
- No `--user`, no resume (no `Range:`), no parallel connections
- No progress bar

## Source

```python
{% include_relative ../../examples/wget.py %}
```

(View on GitHub:
[`examples/wget.py`](https://github.com/avwohl/freedos_micro_python/blob/main/examples/wget.py))
