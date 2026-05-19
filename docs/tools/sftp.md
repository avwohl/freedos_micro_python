---
title: SFTP.PY
---

# `SFTP.PY` — interactive sftp(1)-style shell

Pure-MicroPython SFTP client modelled after OpenSSH's `sftp(1)`.
Connect once, then a prompt accepts `ls / cd / get / put / mkdir
/ rm / mv / stat / exit / ...`. Also has one-shot get/put modes
for shell scripting.

Source: [`examples/sftp.py`](https://github.com/avwohl/freedos_micro_python/blob/main/examples/sftp.py).

## Usage

### Interactive

```
MP.EXE SFTP.PY user@host
sftp> ls
sftp> cd /var/log
sftp> get messages
sftp> exit
```

### One-shot

```
MP.EXE SFTP.PY host:/etc/motd MOTD.TXT     REM auto-dispatched get
MP.EXE SFTP.PY DATA.BIN host:/uploads      REM auto-dispatched put
MP.EXE SFTP.PY get host:/etc/motd MOTD.TXT REM explicit
MP.EXE SFTP.PY put DATA.BIN host:/uploads  REM explicit
```

### Batch

```
MP.EXE SFTP.PY -b BATCH.TXT user@host
```

Where BATCH.TXT contains one sftp> command per line.

### Flags

- `-P PORT` — SSH port (default 22)
- `-u USER` — login user
- `-p PASS` — password
- `-b FILE` — batch mode

### Commands

| Command                  | Effect                              |
|--------------------------|-------------------------------------|
| `ls [path]`              | list remote directory               |
| `cd [path]`              | change remote dir (default `/`)     |
| `lcd [path]`             | change local dir prefix             |
| `pwd` / `lpwd`           | print remote / local cwd            |
| `get remote [local]`     | download                            |
| `put local [remote]`     | upload                              |
| `mkdir path [mode]`      | create remote directory             |
| `rmdir path`             | remove empty remote directory       |
| `rm path` (alias `del`)  | remove remote file                  |
| `mv old new`             | rename remote                       |
| `chmod MODE path`        | (stub — libssh2_sftp_setstat not yet wrapped) |
| `stat path`              | print remote file attrs             |
| `realpath path`          | canonicalize remote path            |
| `exit` / `quit` / `bye`  | disconnect                          |
| `help` / `?`             | show this list                      |
| `# ...`                  | comment (ignored)                   |

### Network setup

Same as [`WGET.PY`](wget.md).

## How it works

The interactive shell:

1. Connects via SSH (same as SCP.PY)
2. Opens the SFTP subsystem: `sftp = sess.sftp()`
3. Reads commands from stdin or batch file
4. Dispatches to the appropriate `_ssh.SFTP` method

For `ls`, calls `sftp.opendir(path)` + iterates entries with
`.read()`.
For `get`, calls `sftp.open(remote, 'r').read()`, writes locally.
For `put`, reads local, calls `sftp.open(remote, 'w').write(data)`.
Etc.

## Limits

- Password auth (publickey via API, not from CLI yet)
- `chmod` not yet wired (libssh2_sftp_setstat is a TODO)
- Single-session — no multi-host / SOCKS proxy / jump host
- Whole files loaded into memory for `get`/`put` (no streaming)
- DOS 8.3 local filenames apply (see
  [DOS file I/O](../dos-features.md))

## Source

View on GitHub:
[`examples/sftp.py`](https://github.com/avwohl/freedos_micro_python/blob/main/examples/sftp.py)
