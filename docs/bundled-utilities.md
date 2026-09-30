---
title: Bundled networking utilities
---

# Bundled networking utilities

The port ships three pure-MicroPython programs that double as
regression tests and as usable standalone tools — drop them into a
DOS image (or run them in the REPL) and they work end-to-end against
real servers.

## Running a program

```
MP.EXE SCRIPT.PY [args ...]
```

runs `SCRIPT.PY` and exits; the remaining words land in `sys.argv`.
Exit status is 0, or 1 on an uncaught exception. With no argument
`MP.EXE` starts the interactive REPL.

You can also paste a program straight into the REPL: press `Ctrl-E`,
paste, then `Ctrl-D`. This needs no file at all, which makes it the
reliable option in the environment noted below.

> **Reading files from disk works.** Verified with one binary on
> QEMU + FreeDOS, DOSBox-X and dosiz: `MP.EXE SCRIPT.PY` with
> `sys.argv`, `open()` / `read()` / `write()` / append, `import` of a
> `.py`, and the `os` / `shutil` calls.
>
> The build bundles the DOS/32A extender. PMODE/W is still selectable
> with `--extender=pmodew`, but its real-mode call path hangs on any
> DOS call that touches a physical sector — see
> [`docs/WIP.md`](WIP.md) item 2.

- **[`examples/wget.py`](https://github.com/avwohl/freedos_micro_python/blob/main/examples/wget.py)** — HTTPS streaming
  downloader. Built on `socket` (lwIP-backed) and `ssl` (axtls
  CERT_REQUIRED supported via `--ca-certs`). Streams in 4 KB chunks
  so the whole body never sits in RAM. Follows up to 5 redirects.
  ```
  MP.EXE WGET.PY -O OUT.TXT https://example.com/file
  ```

- **[`examples/scp.py`](https://github.com/avwohl/freedos_micro_python/blob/main/examples/scp.py)** — SCP client wrapping
  `_ssh.Session.scp_recv()` and `_ssh.Session.scp_send()` (which
  bind libssh2's `scp_recv2` / `scp_send_ex`). Password auth only
  for now; up/down inferred from which arg has the `host:/path` colon.
  ```
  MP.EXE SCP.PY user@10.0.2.2:/etc/motd MOTD.TXT
  MP.EXE SCP.PY DATA.BIN user@10.0.2.2:/uploads/data.bin
  ```

- **[`examples/sftp.py`](https://github.com/avwohl/freedos_micro_python/blob/main/examples/sftp.py)** — SFTP client wrapping
  `_ssh.Session.sftp()` + `SFTP.open()` / `SFTPFile.read|write|close`.
  ```
  MP.EXE SFTP.PY get user@10.0.2.2:/etc/hostname HOST.TXT
  MP.EXE SFTP.PY put REPORT.TXT user@10.0.2.2:/incoming/report.txt
  ```

All three run inside the SSH rig harness (`rigs/ssh-rig/`,
`rigs/tls-rig/`) against a paramiko/local-server fixture and confirm
PASS end-to-end; see [`docs/TESTS.md`](https://github.com/avwohl/freedos_micro_python/blob/main/docs/TESTS.md) for the full
catalog.

Each tool has its own manual page: [`wget.py`](tools/wget.md), [`scp.py`](tools/scp.md), [`sftp.py`](tools/sftp.md).
