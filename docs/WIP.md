# WIP — SSH/SCP under freedos_micro_python

Snapshot at HEAD `a81028c` (freedos) + `10b4dfd` (uc386).

**SSH end-to-end works.** SSHTEST.PY against the paramiko fixture
runs handshake_ok → auth_ok → `exec('echo SSH_RIG_OK')` → len=11
SSH_RIG_OK → PASS, with the test rig exiting cleanly (rc=0).

## What works

- **libssh2 1.11.1 + TweetNaCl** fetched into `upstream/lib/{libssh2,tweetnacl}/`.
- **Crypto adapter** (`port/libssh2_axtls.{h,c}`) maps libssh2's `crypto.h`
  API to axtls (SHA1/256/384/512, MD5, HMAC, AES-CBC, AES-CTR) + TweetNaCl
  (Curve25519 KEX, Ed25519 verify). RSA / DH / key-parse paths are stubs.
- **End-to-end SSH from FreeDOS guest to paramiko server** —
  `rigs/ssh-rig/run-ssh-rig.sh` performs handshake, password auth,
  exec, and verifies the marker. KEX = curve25519-sha256, hostkey =
  ssh-ed25519, cipher = aes256-ctr, MAC = hmac-sha2-256.

## The four fixes that turned the corner this session

1. **uc386 codegen — `alloc_local` cache rebind didn't bump
   `frame_size`** (uc386 commit `10b4dfd`). A user local allocated
   on the collect-pass at `[ebp-N]` and re-bound on the emit-pass
   via the `decl_disps` cache silently kept `frame_size` at the
   pre-bind value, so the next compiler-generated alloc in the
   same scope (e.g. `__compll_addr_*` from `_compound_assign_ll`)
   would start from `frame_size = 0` and land on top of the user
   local. The concrete bite was crypto-algorithms' `sha256_update`:
   the `for (WORD i = 0; …) { ctx->bitlen += 512; }` loop got its
   `i` clobbered by the i64 compound-assign address slot once the
   first transform fired (every 64 bytes). Single-block hashes
   were correct; everything multi-block (incl. the 1362-byte SSH
   exchange hash) was wrong. With the fix, `frame_size` advances
   to `-disp` on cache hit so subsequent allocs land below the
   cached slot. The existing `fetch.sh` tweetnacl/axtls
   compound-assign workarounds are now redundant in principle —
   not yet retired (haven't audited every site).

2. **axtls AES-CTR byte order in `libssh2_axtls.c`.** axtls's
   `AES_encrypt(uint32_t *data, …)` reads its words as big-endian
   (`(w >> 24) & 0xFF` etc.). The CTR path cast a raw `uint8_t[16]`
   counter straight to `uint32_t *` and called `AES_encrypt`
   without byteswapping; the keystream came back per-word reversed.
   Server (paramiko) saw garbage and threw "Invalid packet
   blocking" on the first encrypted SSH packet. Mirror the axtls
   `AES_cbc_encrypt` shape — `ntohl` the counter words into host
   order before AES, `htonl` back after — and the cipher matches.

3. **AES-CTR + `AES_convert_key`.** The same path called
   `AES_convert_key` on the receive-direction context. CTR encrypts
   the counter in both directions, so the inverse key schedule
   produces wrong keystream; only CBC decrypt wants it. Skip the
   call when `is_ctr`.

4. **EOF-as-EAGAIN at the libssh2 recv callback** (modssh). After
   `echo … ; exit` the server sends CHANNEL_DATA + EXIT_STATUS +
   EOF + CLOSE and then shuts down its TCP write side. lwIP's
   `recv()` returns 0 on that FIN; libssh2's transport layer maps
   `recv() == 0` to `SOCKET_RECV` and bails, even when it has
   already-parsed channel packets sitting in `session->packets`.
   Return `-EAGAIN` instead so libssh2 falls through to the
   queue-drain branch in `_libssh2_channel_read`; the buffered
   CHANNEL_DATA reaches us, and the next read after the drain
   returns 0 cleanly. The exec loop also breaks gracefully on
   recv errors once any data has been captured.

## Diagnostics surviving in the source

Light, always-on print markers stayed in:

- `port/modssh_uc386dos.c` — `[s:0]..[s:Z]` in `Session.__init__`.
- `port/libssh2_axtls.c` — `[cv:nE]/nM/kE/kM/kB`, `[ed:nP]/nPok/vE`,
  `[K:NNN]`, `[H:NNN]`, `[hu:LLLL:BB]` (per-update size + first
  byte for the exchange-hash SHA256).
- `rigs/ssh-rig/ssh_server.py` — paramiko monkey patches that log
  raw recv bytes, decoded packet types, and exception types.

Heavier debug output (full-byte chunk dumps, AES IV/keystream,
lwip send markers, channel_read result markers) was removed after
the test passed.

## Things done since the SSH-working snapshot

  - **Retired the tweetnacl compound-assign workarounds in
    `fetch.sh`** (commit `e49134a`). uc386 commit `10b4dfd` made
    them dead weight. Patch shrank from 7 file-static hoists + 12
    rewrites + 10 modL/SHA512 sed substitutions down to just the
    7 hoists. SSHTEST still PASS; 102/103 smoke tests pass.
  - **NE2K-probe early-out in `pktdrv_uc386dos.c`** (commit
    `bd4e4cc`). The PM-native NE2K path that landed in `be1985e`
    crashed in dos_emu's NetworkSimulator (no NE2K I/O port
    emulation) on `rep insw`. The probe writes CR=0x21, reads
    back; on mismatch (0xFF), returns -1 so pktdrv_init falls
    through to the Crynwr path the netsim does emulate. Restores
    3 lwip smoke tests (DHCP, DHCP int83 fallback, DNS query).
  - **TLS rig also works again** without any TLS-specific change:
    `rigs/tls-rig/run-tls-rig.sh` now exits rc=0 with
    `TLSTEST: PASS`, `data_len 25`. The same uc386 + multi-block
    SHA / AES fixes that unblocked SSH were the underlying issue.
  - **SFTP round-trip working end-to-end** (commit `1f2c7b5`).
    `session.sftp()`, `SFTP.open(path, mode)`, `SFTPFile.read/write/close`
    wired to libssh2's SFTP API. The rig downloads then uploads a
    marker through the same SSH session it ran exec on, against a
    paramiko `SFTPServer` backed by an in-memory dict. Five fixes
    had to land together: (1) `_libssh2_wait_socket` patched to
    drive lwIP's poll hook + a 50 ms delay instead of `select()`
    on our fake fd, (2) socket settimeout(0.1) + recv-callback
    maps ETIMEDOUT→EAGAIN so `channel_write`'s drain-incoming
    loop can exit, (3) `tcp_output_nagle` → `tcp_output` so small
    encrypted SSH packets aren't held by Nagle/delayed-ACK,
    (4) `ssh_server.py` registers `SFTPServer` via
    `set_subsystem_handler`, (5) `SSHTEST.PY` PASS print is
    leading-`\n` so the rig's column-anchored grep catches it.
  - **SCP round-trip working end-to-end** (commit `3258120`).
    `session.scp_recv(path)` + `session.scp_send(path, mode, data)`
    wrap `libssh2_scp_recv2` / `libssh2_scp_send_ex`. The rig
    downloads + uploads in the same session that ran exec and SFTP,
    against a small in-process SCP server inside `ssh_server.py`
    (handles both `-t` / `-f` plus the `-p` T-line branch). Three
    fixes landed together: (1) libssh2's SCP C-line snprintf uses
    `"C0%o %lld %s\n"` which uc386's snprintf garbles (`%o` and
    `%lld` both eaten without reading args) — patched via
    `patch_libssh2_scp_int64_format` in `fetch.sh` to hardcode
    `"0644"` and use `"%ld"` with a `(long)` cast on size,
    (2) `scp_recv` passes `NULL` for `sb` so libssh2 omits `-p`
    and the server doesn't need to send a T-line first; recv reads
    until CHANNEL_EOF or 20 EAGAINs with data buffered (paramiko
    sometimes drops the post-exec EOF), (3) `scp_send` writes the
    SCP trailing `\0` after the payload before `send_eof` —
    omitting it leaves the server unsure when the file ended and
    server-side persistence was bailed.

## Things to fix next

1. **Real RSA / DH / key-parse implementations** in
   `port/libssh2_axtls.c`. Currently stubs returning `-1`. Needed
   for non-ed25519 server keys and DH-group KEX fallback.

2. **Public-key auth** — `session.userauth_publickey(user, key)`
   wraps `libssh2_userauth_publickey_fromfile_ex`. Today only
   password auth is exposed.

3. **Expose POSIX TCP_NODELAY in modlwip.** modlwip's `case
   TCP_NODELAY:` switch matches on the lwIP `TF_NODELAY = 0x40`
   constant, not POSIX `TCP_NODELAY = 1`. Currently nobody needs
   to set NODELAY (SSHTEST works without it after the EOF→EAGAIN
   fix), so the gap is cosmetic — but it'll bite anyone porting
   a POSIX socket program. Upstream MicroPython fix: either
   expose `socket.TCP_NODELAY` as a module constant equal to
   `TF_NODELAY`, or translate POSIX 1 → TF_NODELAY inside the
   setsockopt handler.

## Run

```sh
cd ~/src/freedos_micro_python/rigs/ssh-rig
./run-ssh-rig.sh
# expect: rig rc=0, SSHTEST: PASS at the end of qemu-ssh.log
```

## Rebuild

```sh
cd /private/tmp/fdmp-build
~/src/uc386/.venv/bin/freedos-micropython port
# ~25–30 min; produces build/micropython.bin

cd ~/src/uc386
.venv/bin/python -m addons.harness.exe \
  /private/tmp/fdmp-build/build/micropython.asm \
  -o ~/src/freedos_micro_python/rigs/dosbox-x-rig/MP.EXE
```
