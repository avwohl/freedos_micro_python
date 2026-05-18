# Tests + programs in this port

The MicroPython upstream tree ships its own test suite under
`upstream/tests/`. The port adds two layers on top:

1. **Programs that are useful in their own right** — small MicroPython
   apps that double as integration tests for the harder bits of the
   port (HTTPS, TLS verify, SSH KEX + auth + exec).
2. **Pytest suite that drives `MP.EXE` under dos_emu** — 100+ tests
   that exercise the standard library and the port-specific
   modules (lwIP, axtls, `_ssh`, `uc386_net`, time/RTC,
   filesystem stubs, etc.).

Together they cover the surface that upstream's test suite doesn't
reach (no DOS/real-mode harness upstream) and the parts of the
standard library that need a working network + crypto stack.

---

## Section 1 — Useful programs

Programs that solve a real task and exist as the canonical end-to-end
test for the hardest paths in the port. They are checked into the
tree and shipped into the QEMU disk image at rig-run time.

### `examples/wget.py` — HTTPS streaming downloader

A 280-line `wget` replacement built against `socket` (lwIP-backed),
`ssl` (axtls-backed), and port-provided `urllib.parse`.

  - HTTP and HTTPS, redirect-following (up to 5 hops).
  - Optional CA bundle for `verify_mode=CERT_REQUIRED`.
  - 4 KB streaming chunks — the response body is never held in RAM
    in full.
  - Run from the REPL (`import wget; wget.main([...])`) or directly
    as `MP.EXE WGET.PY <url>`.

  The matching pytest harness `tests/test_wget.py` runs the same
  module under CPython with stubs for `socket`/`ssl`/`urllib.parse`
  for unit-style coverage of URL parsing, redirect resolution, and
  request-building. The `test_wget_runs_under_micropython` case
  loads `wget.py` into `MP.EXE` and confirms `wget.main(["-h"])`
  prints the help text.

  Exercises end-to-end: `socket` + `ssl` (axtls handshake +
  CERT_REQUIRED verify) + `urllib.parse` + file write to the DOS
  filesystem.

### `rigs/ssh-rig/SSHTEST.PY` — SSH client (KEX → auth → exec)

A 58-line standalone program that performs a full SSH-2 session:

  1. Bring up the NE2000 NIC via `uc386_net`.
  2. Static-IP `10.0.2.15` and TCP-connect to the paramiko fixture
     at `10.0.2.2:2222`.
  3. `_ssh.Session(s)` — curve25519-sha256 KEX, ssh-ed25519
     hostkey, aes256-ctr, hmac-sha2-256.
  4. `sess.userauth_password('testuser', 'testpass')`.
  5. `sess.exec('echo SSH_RIG_OK')` and verify the marker.

  Exercises: TweetNaCl (X25519, Ed25519 verify), axtls
  (SHA256, HMAC-SHA256, AES-256-CTR), libssh2 1.11.1
  (transport, KEX, channel, userauth), lwIP TCP, the port's
  `_ssh` and `uc386_net` C modules, and the
  `freedos-micropython` build pipeline end-to-end.

  Run: `cd rigs/ssh-rig && ./run-ssh-rig.sh` — expect `rig rc=0`
  and `SSHTEST: PASS`. The rig now also exercises SFTP
  (download + upload) and SCP (download + upload) over the same
  session, see the SCP/SFTP standalone wrappers below.

### `examples/scp.py` — SCP up/download

A small `scp` client (~120 lines) wrapping
`_ssh.Session.scp_recv()` / `scp_send()`. Direction is inferred
from which positional arg has the `host:/path` colon. Today only
password auth is wired up (public-key auth waits on real
RSA/DH parsing in `port/libssh2_axtls.c`).

  Run from MP:
  ```
  MP.EXE SCP.PY user@10.0.2.2:/etc/motd MOTD.TXT
  MP.EXE SCP.PY DATA.BIN user@10.0.2.2:/uploads/data.bin
  ```

  Exercises end-to-end: `_ssh` channel I/O (data + trailing-\0
  SCP terminator), libssh2's `scp_recv2` / `scp_send_ex`, and
  the FreeDOS file-write path. The rig at `rigs/ssh-rig/` drives
  a full down + up round-trip against a paramiko-backed SCP
  fixture (in-memory, shared with SFTP), so a green `SSHTEST: PASS`
  covers SCP as well.

### `examples/sftp.py` — SFTP get/put

A small `sftp` client (~110 lines) wrapping `_ssh.Session.sftp()` +
`SFTP.open(path, mode)` + `SFTPFile.read|write|close`. Subcommands
`get` / `put` parallel `scp.py`.

  Run from MP:
  ```
  MP.EXE SFTP.PY get user@10.0.2.2:/etc/hostname HOST.TXT
  MP.EXE SFTP.PY put REPORT.TXT user@10.0.2.2:/incoming/report.txt
  ```

  Exercises end-to-end: SFTP subsystem (channel-level `subsystem`
  request), the libssh2-axtls crypto adapter, and the same paramiko
  fixture the SCP path uses. Covered by the same `SSHTEST.PY` rig
  pass.

### `rigs/tls-rig/{TLSTEST,WGETTEST,READTEST}.PY` — TLS rigs

Companion programs to the TLS-server pytest fixture. `TLSTEST.PY`
drives a full TLS handshake + read; `WGETTEST.PY` shells out to
the local `WGET.PY` against an HTTPS server; `READTEST.PY` covers
the PEM-reader path used by `SSLContext.load_verify_locations`.
`NETBASE.PY` / `READBASE.PY` are the minimum harnesses for
isolating netif and filesystem behavior from the TLS layer.

(The TLS rig is currently regressed — see `docs/WIP.md`. The
programs are correct; the underlying handshake-state machinery
broke and is the next thing to chase.)

---

## Section 2 — Pytest suite we added

`tests/test_smoke.py` runs 103 cases against `MP.EXE` via
`uc386.harness.run()` (the dos_emu wrapper). Each test feeds a
short MicroPython program over stdin, captures stdout under a CPU
budget, and asserts on output markers. The dos_emu path is hermetic
(no QEMU, no NIC), so the whole suite runs in seconds per test on a
build machine.

The pytest layer wraps the standard MP test idiom for our port's
constraints (real-mode DOS, no scheduler, no thread, time and
filesystem semantics that don't match upstream's POSIX assumptions).

### What the 103 tests cover

  Core language + interpreter:
    REPL boot + banner, builtins, control flow, comprehensions,
    `try`/`except`, def/call, decorators, f-strings, special methods,
    f-frozenset, deque, memoryview, compile()/eval(), in-place
    operators, sys.exc_info(), `range` arithmetic, stack-check on
    runaway recursion, `__file__` on imported modules.

  Standard library (import + small smoke):
    `sys` (argv, modules, path, exit), `gc`, `collections`,
    `struct`, `errno`, `math` + `cmath`, `time` (ticks +
    `sleep_ms` + DOS RTC time/localtime/gmtime/time_ns),
    `random` (seeded determinism), `binascii` (hexlify, crc32),
    `hashlib` (md5, sha1, sha256), `deflate`/inflate roundtrip,
    `heapq`, `re` (group capture), `json`, `base64`, `tempfile`,
    `shutil` (copy/move), `uctypes` struct roundtrip, `platform`.

  Filesystem (port-specific DOS FAT bindings):
    `os.mkdir`/`chdir`/`getcwd`, `os.stat`/`listdir`/`rename`/
    `unlink`, `os.path.join`/`split`/`exists`/`getsize`/`isabs`/
    `abspath`/`normpath`, `os.getenv`/`os.environ`, `open` (read
    + write+read roundtrip), import from disk, `os.system` exec.

  Float + long-long arithmetic (uc386-specific):
    `float_arithmetic`, `float_repr_round_trips`, `math_gamma`,
    `math_special_functions` (lgamma, erf, etc.), `math_constants`,
    `math_factorial`, `math_isclose`, `long_long_int_arithmetic`.
    These caught more than one uc386 codegen bug — they're the
    canary suite when the compiler changes.

  Networking (lwIP-backed):
    `lwip_loopback_tcp`, `lwip_dhcp` (NE2000 driver),
    `lwip_dhcp_int83_fallback` (rmstub path), `lwip_dns_query`
    (resolver), `lwip_udp_socket`, `lwip_http_loopback`,
    `select_poll`, `select_module_imports`, `dosbox_x_rig_loads_ne2000`
    (DOS packet driver / NE2000 init under qemu).

  TLS surface (axtls):
    `import_ssl`, `ssl_context_construct`, `ssl_verify_mode_settable`,
    `ssl_load_verify_locations`, `ssl_load_verify_locations_requires_cadata`.
    Real-network TLS handshakes happen in the rig, not pytest, so
    this layer is API-only.

  SSH surface (libssh2 + axtls + TweetNaCl):
    `import_ssh`. Confirms `_ssh.version()` returns `'1.11.1'`
    and `_ssh.crypto_engine_name()` returns `'axtls'`. The full
    end-to-end exec is the SSHTEST.PY rig (Section 1).

  Async + machine:
    `asyncio_taskqueue` (TaskQueue + Task primitives), `machine_module`
    (signal/mem stubs).

`tests/test_wget.py` is the matching unit + integration test for
`examples/wget.py` (11 cases): URL parsing, redirect resolution,
header construction, full TLS round-trip against a local HTTPS
server with cert verify, and one end-to-end run inside `MP.EXE`.

`tests/test_gen_qstrdefs.py` covers the build-time qstr generator
that scans port C sources for `MP_QSTR_*` references.

### Running

```sh
# Build first
cd /private/tmp/fdmp-build && ~/src/uc386/.venv/bin/freedos-micropython port

# Run all smoke tests
cd ~/src/freedos_micro_python
FREEDOS_MP_BIN=/private/tmp/fdmp-build/build/micropython.bin \
  ~/src/uc386/.venv/bin/python -m pytest tests/ -v

# Single test
~/src/uc386/.venv/bin/python -m pytest \
  tests/test_smoke.py::test_micropython_import_ssh -v
```
