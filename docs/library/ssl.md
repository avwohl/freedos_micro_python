---
title: ssl
---

# `ssl` — TLS over `socket`

```python
import ssl
```

TLS 1.2 client + server, backed by [axtls](http://axtls.sourceforge.net/).
Wraps a regular `socket.socket` to add encryption and certificate
verification.

## Quick client

```python
import socket, ssl

s = socket.socket()
s.connect(socket.getaddrinfo('example.com', 443)[0][-1])
ss = ssl.wrap_socket(s, server_hostname='example.com')
ss.write(b'GET / HTTP/1.0\r\nHost: example.com\r\n\r\n')
print(ss.read(4096).decode())
ss.close()
```

`ssl.wrap_socket(sock, *, ...)` returns a wrapped socket that
quacks like a regular one (`.read()`, `.write()`, `.close()`)
but encrypts the wire.

## Certificate verification

By default, the wrapper does NO certificate verification — it
trusts whatever the server presents. To verify against a CA bundle:

```python
ss = ssl.wrap_socket(
    s,
    server_hostname='example.com',
    cert_reqs=ssl.CERT_REQUIRED,
    ca_certs='CACERTS.PEM',           # path to PEM file
)
```

If you don't have a `CACERTS.PEM` handy, Mozilla's bundle (the
one all your other tools use) is at <https://curl.se/ca/cacert.pem>.
Save it as `CACERTS.PEM` next to `MP.EXE`.

`ssl.CERT_NONE` (default) — don't verify.
`ssl.CERT_REQUIRED` — verify; raise `OSError` on failure.

⚠️ On a small DOS system, full chain verification of a real
Internet cert chain uses a few hundred KB of axtls heap. If you're
tight on memory, consider trusting a specific server cert directly
instead of a full CA bundle.

## Server (less common)

```python
srv = socket.socket()
srv.bind(('0.0.0.0', 443))
srv.listen(5)

while True:
    conn, addr = srv.accept()
    ss = ssl.wrap_socket(conn, server_side=True,
                         key='SERVER.KEY', cert='SERVER.PEM')
    ss.write(b'HTTP/1.0 200 OK\r\n\r\nHello!\n')
    ss.close()
```

## SSLContext

```python
ctx = ssl.SSLContext()
ctx.load_verify_locations(cafile='CACERTS.PEM')
ctx.verify_mode = ssl.CERT_REQUIRED

ss = ctx.wrap_socket(sock, server_hostname='example.com')
```

Same effect as the kwargs version, but reusable across many
connections.

## Cipher suites

axtls negotiates:

- Key exchange: RSA (P12-style key files), ECDHE on Curve25519
- Bulk: AES-128/256-CBC, AES-128/256-GCM, ChaCha20-Poly1305 NOT
  supported (chachapoly is stubbed in our axtls adapter)
- MAC: HMAC-SHA256, HMAC-SHA1

The negotiated suite is logged with `ssl.SSLContext.set_verify_mode`
debug builds; users normally don't need to look.

## See also

- [`socket`](socket.md) — the underlying transport
- [`_ssh`](_ssh.md) — SSH client (separate cryptography stack:
  axtls + TweetNaCl + libssh2)

---

*Credit: shape from
[MicroPython ssl docs](https://docs.micropython.org/en/latest/library/ssl.html)
(MIT). axtls notes are this port's.*
