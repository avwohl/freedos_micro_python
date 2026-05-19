---
title: Troubleshooting
---

# Troubleshooting

## `MP.EXE` says nothing and exits

PMODE/W couldn't bring up protected mode. Common causes:

- **HIMEM not loaded.** PMODE/W needs XMS. Most modern FreeDOS
  setups load `HIMEMX.EXE` from `CONFIG.SYS` by default.
- **EMM386 with NOEMS.** Try with EMM386 unloaded. PMODE/W is
  picky about coexisting with VCPI clients.
- **Already in PM.** A DPMI host (CWSDPMI etc.) already loaded?
  PMODE/W will detect and use it via AX=1687, so this usually
  isn't a problem — but if you've layered DOSBox-X on top of
  something that's already PM, you'll see it.

Add `set MP_DEBUG=1` and watch for `[mp-main-entered]`. If you see
the bridge markers (`[bridge-entered]`, `[bridge-pre-call-main]`,
…) but not `[mp-main-entered]`, the PM transition worked and the
issue is in main().

## `OSError: ENOENT`

DOS couldn't find the file. Check 8.3 case + extension:

```python
import os
print(os.listdir())     # what's actually there?
```

Remember `.JSON` truncates to `.JSO` on save; same for any 4-char
extension.

## `OSError: 22` (`EINVAL`) on `open()`

Path contains a character DOS doesn't allow. Forward slashes are
OK; literal `/`, `\`, `:`, `*`, `?`, `"`, `<`, `>`, `|` are
disallowed in filename portions.

## `socket.connect()` raises `OSError: 110` (`ETIMEDOUT`)

Three checks, in order:

1. **Packet driver running?** `os.getenv('PATH')` won't tell you
   — try `uc386_net.eth_init(True)` and look for the chatty
   "found NE2000 at ..." output, or "Crynwr packet driver
   at INT 0x60".
2. **IP / mask / gateway right?** `uc386_net.eth_status()`. If
   the IP is `0.0.0.0`, DHCP failed and you've got nothing.
3. **Destination listening?** From DOS you have no easy `ping` —
   try connecting to a known-up port (your host's SSH at 22, or
   `10.0.2.2:22` under qemu).

## `OSError: 9` (`EBADF`) on TCP recv

Peer closed the connection. Normal for HTTP responses that don't
send `Content-Length` — read until you get `b''`, then close
your end.

## REPL is stuck after pasting a multi-line block

You're in continuation mode (`...` prompt). Press **Ctrl-C** to
cancel, or paste **Ctrl-D** to terminate paste mode if you used
Ctrl-E to start it.

Or: hit Enter on a blank line to terminate a `def` / `for` / `if`.

## SSH handshake hangs

Make sure your socket has a timeout BEFORE you build the
`_ssh.Session`:

```python
s = socket.socket()
s.settimeout(0.1)         # ← essential
s.connect(...)
sess = _ssh.Session(s)
```

Without a positive timeout, libssh2's drain-incoming loop waits
forever for packets that may not arrive.

## SSH refuses the host key

This port only knows `ssh-ed25519` host keys. RSA / ECDSA hosts
fail. On the server, regenerate or just add an Ed25519 host key:

```
$ ssh-keygen -t ed25519 -f /etc/ssh/ssh_host_ed25519_key -N ''
```

## `open()` hangs after `import _ssh`

Known issue under PMODE/W in DPMI 0x0301 dispatch. Workarounds:

- **Inline at build time**: see how `rigs/ssh-rig/SSHTEST.PY`
  uses a `__CLIENT_KEY_BYTES__` placeholder substituted by
  `run-ssh-rig.sh` before launch.
- **Do the I/O before any `_ssh` import** — though even this can
  be triggered by enough other module imports. The conservative
  workaround is the inline pattern.

Tracked in [`docs/WIP.md`](WIP.md) under "Things to fix next" #2;
partial fix landed in commit `ed43d0a` (stack-switching INT 31
dispatcher).

## `print(int)` aborts on dosiz

Need dosiz @ commit
[`237339d`](https://github.com/avwohl/dosiz/commit/237339d) or
later. Older dosiz set the int 0x80 IDT gate to 16-bit because of
a `cpu.code.big` race, mis-aligning the bridge's iretd frame.

## OutOfMemory during TLS handshake

axtls's cert-chain verification can take 200–300 KB of PMODE/W
heap. Possible mitigations:

- Skip verification: omit `--ca-certs` / `cert_reqs=CERT_REQUIRED`
  (only safe for known-trusted networks).
- Trust a single end-cert directly instead of a full root chain:
  save the server's cert as a single PEM and use that as
  `ca_certs`.
- `gc.collect()` before the handshake (frees up the Python heap
  but doesn't help axtls's separate heap).

## NASM phase error during build

The combined .asm output sometimes hits NASM's short-vs-long
jump convergence warnings. Build_port.sh suppresses them with
`-w-error=label-redef-late`; if you're calling NASM manually,
add the same flag.

## `freedos-micropython port` says `qstrdefs.generated.h` missing

Run `freedos-micropython build` first. The build step generates
the qstr table; `port` only runs the full multi-TU compile.

If you've added a new MP_QSTR reference and the port build fails
with `unexpected token IDENT 'qstr_xxx'`, delete
`build/genhdr/qstrdefs.generated.h` to force regeneration. Build
script caches it for incremental speed.

## More

Open an issue at
<https://github.com/avwohl/freedos_micro_python/issues> with:

- What you ran (full command, full Python script)
- What you saw (full output / traceback)
- What you expected
- DOS environment (real hw / dosbox-x / qemu+FreeDOS / dosiz)
- MP.EXE version (`>>> sys.version`)
