---
title: SCP.PY
---

# `SCP.PY` — SCP client

A pure-MicroPython SCP-1 client wrapping
[`_ssh.Session`](../library/_ssh.md). Transfers single files
between DOS and a remote server using SSH (`scp` protocol).

Source: [`examples/scp.py`](https://github.com/avwohl/freedos_micro_python/blob/main/examples/scp.py).

## Usage

```
MP.EXE SCP.PY [-P PORT] [-u USER] [-p PASS] SRC DST
```

Direction is auto-detected from which arg has the `host:/path`
colon.

### Examples

```
REM Download:
MP.EXE SCP.PY user@10.0.2.2:/etc/motd MOTD.TXT

REM Upload:
MP.EXE SCP.PY DATA.BIN user@10.0.2.2:/uploads/data.bin

REM Explicit user / port / password:
MP.EXE SCP.PY -P 2222 -u alice -p secret SRC.TXT alice@host:/dst.txt
```

### Flags

- `-P PORT` — SSH port (default 22)
- `-u USER` — login user (default `testuser` for the rig)
- `-p PASS` — password (default `testpass` for the rig)

The default user / pass match the bundled SSH rig fixture
(`rigs/ssh-rig/ssh_server.py`) so the example works out of the box
against the rig server.

### Path conventions

- `user@host:/path` — absolute path on remote
- `host:/path` — defaults user to flag value
- `path` (no colon) — local file

### Network setup

Same as [`WGET.PY`](wget.md) — bring lwIP up first.

## How it works

1. Parse args → src / dst, infer direction
2. Open TCP socket to `host:port`, `settimeout(0.1)`
3. `sess = _ssh.Session(s)` — handshake
4. `sess.userauth_password(user, password)`
5. Either `data = sess.scp_recv(remote_path)` and write local, or
   read local and `sess.scp_send(remote_path, mode, data)`
6. `sess.close()`

## Limits

- Single file per invocation (no `-r` recursive)
- Password auth only (Ed25519 publickey works at the API level —
  the bundled script doesn't expose it as a flag yet; see the
  `rigs/ssh-rig/SSHTEST.PY` for the publickey pattern)
- Mode hardcoded to `0644` for `scp_send` (a libssh2 snprintf
  formatter bug under uc386 that we patched around — see
  [WIP.md](../WIP.md))
- The remote must speak SCP protocol (most do; OpenSSH does, dropbear does)

## Source

View on GitHub:
[`examples/scp.py`](https://github.com/avwohl/freedos_micro_python/blob/main/examples/scp.py)
