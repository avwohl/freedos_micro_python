---
title: _ssh
---

# `_ssh` — SSH client (libssh2 + axtls + TweetNaCl)

```python
import _ssh
```

A real SSH-2 client. Backed by [libssh2](https://www.libssh2.org/)
on top of axtls (symmetric crypto, hashes) + TweetNaCl
(Curve25519 KEX, Ed25519 host-key verify).

Supports:

- **KEX**: Curve25519-SHA256
- **Host key**: ssh-ed25519
- **Cipher**: AES-128/256-CTR
- **MAC**: HMAC-SHA-256 / HMAC-SHA-1
- **User auth**: password, Ed25519 publickey (from in-memory bytes)
- **Channels**: `exec`, SCP (`scp_recv` / `scp_send`), full SFTP

RSA / DH-group KEX / ECDSA-NIST host keys are NOT supported (the
axtls backend stubs return -1). Hosts that only offer those will
fail the handshake.

## Module-level

### `_ssh.version()` — libssh2 version string

```python
>>> _ssh.version()
'1.11.1'
```

### `_ssh.crypto_engine_name()` — `'axtls'`

```python
>>> _ssh.crypto_engine_name()
'axtls'
```

## `_ssh.Session(sock)` — the connection

Constructor takes a connected `socket.socket`. Performs the
handshake immediately.

```python
import socket, _ssh

s = socket.socket()
s.connect(socket.getaddrinfo('10.0.2.2', 22)[0][-1])
s.settimeout(0.1)                 # essential — see "Quirks"
sess = _ssh.Session(s)
```

### `.userauth_password(user, password)`

Password auth. Raises `OSError` on failure.

```python
sess.userauth_password('testuser', 'testpass')
```

### `.userauth_publickey(user, privkey_bytes, pubkey_bytes=None, passphrase=None)`

Ed25519 publickey auth. `privkey_bytes` is the contents of an
OpenSSH-format `id_ed25519` file. `pubkey_bytes` is optional —
libssh2 derives it from the private key if omitted.

```python
with open('CLIENT.KEY', 'rb') as f:
    privkey = f.read()

sess.userauth_publickey('me', privkey)
```

(See ["`open()` hangs after `import _ssh`"](../troubleshooting.md#open-hangs-after-import-_ssh) for
a current `open()` gotcha that the bundled `sftp.py` etc. work
around with build-time inlining.)

### `.exec(command)` — run a command, return stdout

```python
out = sess.exec('uname -a')
print(out.decode())             # bytes → str
```

Runs to completion, returns full stdout. Stderr / exit-status not
captured.

### `.sftp()` — open SFTP subsystem

Returns an `SFTP` object. See `SFTP` below.

### `.scp_recv(remote_path)` → `bytes`

Pulls a single file from the server, returns the bytes.

```python
data = sess.scp_recv('/etc/motd')
print(data.decode())
```

### `.scp_send(remote_path, mode, data)` → bytes written

Pushes a single file to the server.

```python
sess.scp_send('/uploads/hello.txt', 0o644, b'hello from DOS\n')
```

### `.close()`

Close the session.

## `SFTP` — `sess.sftp()`

### `.open(path, mode='r')` → `SFTPFile`

Modes: `'r'`, `'w'`. Returns a file-like with `.read()`, `.write(data)`,
`.close()`.

```python
sftp = sess.sftp()
f = sftp.open('/etc/hostname', 'r')
print(f.read().decode())
f.close()

g = sftp.open('/tmp/from-dos.txt', 'w')
g.write(b'hello\n')
g.close()
```

### `.opendir(path)` → `SFTPDir`

`.read()` returns `(name, attrs)` tuples; `None` at end-of-dir.

```python
d = sftp.opendir('/etc')
while True:
    entry = d.read()
    if entry is None: break
    name, attrs = entry
    mode, size, atime, mtime, uid, gid = attrs
    print(f'{mode:o} {size:>10} {name}')
d.close()
```

### `.stat(path)` → 6-tuple

`(mode, size, atime, mtime, uid, gid)`.

### `.realpath(path)` → `str`

Resolve to absolute canonical path.

### `.mkdir(path, mode=0o755)` / `.rmdir(path)` / `.unlink(path)` / `.rename(old, new)`

Standard SFTP file ops.

### `.close()`

## Quirks

- The underlying `socket` must have `settimeout(0.1)` (or some
  small positive). Without a timeout, the SSH transport's
  drain-incoming loop blocks forever waiting for packets that
  won't come. Verified empirically.
- After password auth on one socket, opening a second SSH session
  needs a fresh TCP connect — paramiko's transport won't re-auth
  on the same connection.
- `Session.close()` must be called before reconnecting; otherwise
  libssh2's session state leaks.
- For pubkey auth from a file, see "Where credentials live on
  DOS" below.

## Where credentials live on DOS

DOS has no `$HOME` and no `~/.ssh/`, so the usual OpenSSH paths do
not exist. There is also no fallback location built into this
port — nothing is searched automatically. You pass paths
explicitly, and the conventions below are what the examples and
docs assume.

**Recommended layout.** Put credentials in an `\SSH\` directory
at the root of whatever drive you run from:

```
C:\SSH\ID_ED.KEY      private key   (OpenSSH format)
C:\SSH\ID_ED.PUB      public key
C:\SSH\KNOWN_H.TXT    known hosts
```

**8.3 names are mandatory.** FAT gives you eight characters plus a
three-character extension, uppercased. The OpenSSH names do not
survive:

| OpenSSH | On DOS |
|---|---|
| `id_ed25519` | `ID_ED.KEY` |
| `id_ed25519.pub` | `ID_ED.PUB` |
| `known_hosts` | `KNOWN_H.TXT` |
| `authorized_keys` | `AUTH_KEY.TXT` |

Pick the names you like — nothing in the port depends on these —
but write them the way DOS will actually store them. A file
copied in as `id_ed25519` becomes something like `ID_ED25.519`,
which is not what your script will be asking for.

**Pointing the examples somewhere else.** `examples/scp.py` and
`examples/sftp.py` take the path as an argument; there is no
default and no search path. Give a full path including the drive
letter if you are not running from the same drive.

**Current status — read this before planning around it.** Only
password auth is wired up. Public-key auth needs the RSA / key-
parse implementations in `port/libssh2_axtls.c`, which are still
stubs returning `-1` (docs/WIP.md item 1), and
`userauth_publickey_fromfile` is not bound (item 3). So the table
above is the convention to write *to*, not something the port
reads today.

**Interim recipe: inline the key at build time.** Until key
loading lands, the working pattern — the one `rigs/ssh-rig` uses
and passes with — is to substitute the key bytes into the script
before it reaches DOS, rather than reading a file at runtime:

```python
# SSHTEST.PY carries a placeholder that the rig replaces:
PRIVKEY = __CLIENT_KEY_BYTES__       # b"-----BEGIN OPENSSH..."
session.userauth_publickey(user, PRIVKEY)
```

The rig renders that placeholder from the real key file on the
build host, then copies the rendered script onto the disk image.
See `rigs/ssh-rig/run-ssh-rig.sh`.

Note this workaround exists because key *parsing* is missing, not
because file reading is broken. Reading a key file works wherever
disk I/O works — verified under DOSBox-X. On QEMU + FreeDOS it
will hit the disk wedge in docs/WIP.md item 2.

## See also

- [`scp.py`](../tools/scp.md) — command-line wrapper
- [`sftp.py`](../tools/sftp.md) — interactive sftp shell
- [`socket`](socket.md), [`ssl`](ssl.md)

---

*Credit: API shape original; libssh2 protocol terminology from
[libssh2 docs](https://libssh2.org/docs.html).*
