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
  - **Ed25519 publickey auth** (commits `f0a12d9` + `506efe0`).
    `session.userauth_publickey(user, privkey_bytes[, pubkey_bytes,
    passphrase])` wraps `libssh2_userauth_publickey_frommemory_ex`;
    privkey is the in-memory contents of an OpenSSH-format
    `id_ed25519` (the unencrypted `-----BEGIN OPENSSH PRIVATE
    KEY-----` block). `port/libssh2_axtls.c`'s ed25519
    sign/load-private stubs are replaced with real impls: signing
    calls TweetNaCl's `crypto_sign` and returns the leading 64-byte
    signature; loading walks libssh2's own
    `_libssh2_openssh_pem_parse_memory` and copies the 32-byte
    pub + 64-byte priv into a `libssh2_ed25519_ctx`. The same
    parse builds the wire-format pubkey blob for
    `_pub_priv_keyfilememory`, so `libssh2_userauth_publickey_frommemory`
    can derive the pubkey from the private alone. Stubs
    also added for `_libssh2_supported_key_sign_algorithms`
    (returns NULL — no RSA-SHA2 upgrade) and `_libssh2_bcrypt_pbkdf`
    (returns -1 — encrypted privkeys not supported yet, pem.c
    surfaces a clean decrypt failure). The file-based variants
    (`_new_private` / `_pub_priv_keyfile`) stay stubbed since uc386
    has no fopen wired into libssh2; user code reads the file via
    MicroPython's `open()` and passes bytes through. Rig coverage:
    `run-ssh-rig.sh` generates a per-rig client key and inlines
    its bytes into SSHTEST.PY at the `__CLIENT_KEY_BYTES__`
    placeholder (the test can't `open('CLIENT.KEY')` after
    `import _ssh` — DOS INT 21h AH=3D hangs in the DPMI 0x0301
    thunk from the paste-mode REPL, root cause not yet pinned).
    SSHTEST opens a 2nd socket+Session after the password path
    closes, runs `userauth_publickey('pkuser', CLIENT_PRIVKEY)`,
    execs `echo PUBKEY_RIG_OK`, and gates PASS on the marker.
    `_handle_exec` in `ssh_server.py` is now a tiny real `echo`
    impl so password and pubkey paths can ask for different
    markers; `serve_one` accepts sequential connections so the
    same server can serve both sessions.

  - **SFTP filesystem-ops surface** (commits `09064fe` +
    `8eadb57`). `session.sftp()` now exposes `opendir` (→ `SFTPDir`
    with `.read()` → `(name, attrs)` | `None`), `mkdir`, `rmdir`,
    `unlink`, `rename`, `stat`, `realpath` on top of the existing
    `open`/`read`/`write`. Attrs surface as a 6-tuple
    `(mode, size, atime, mtime, uid, gid)` with absent fields
    zeroed. Rig coverage: `_InMemorySFTPServer` in `ssh_server.py`
    grows `stat`/`list_folder`/`mkdir`/`rmdir`/`remove`/`rename`
    impls over the same in-RAM dict + parallel `dirs` set, and
    SSHTEST.PY's new `sftp_fsops` slice round-trips
    realpath → opendir → stat → mkdir → rename → unlink → rmdir
    against `/sftp` in the same SSH session. PASS now gates on it.
    `examples/sftp.py` was rewritten as an interactive `sftp(1)`-
    style shell (`sftp> ls / cd / get / put / mkdir / ...`)
    consuming the new surface; old `get`/`put` subcommand form +
    auto-dispatched `host:/path local` shorthand kept for
    back-compat. `chmod` stubs out — `libssh2_sftp_setstat` isn't
    wrapped yet.
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

   Assessed, not started. The primitives are already in the tree —
   `upstream/lib/axtls/crypto/rsa.c` and `bigint.c` — and the stub
   signatures map onto them almost 1:1, so this is ordinary work
   rather than a research problem. Slices in value order:

   - **(a) RSA host-key verification.** The one that actually
     unblocks users, since most servers still present an RSA host
     key. `_libssh2_axtls_rsa_new` with only `edata`/`ndata`
     populated is `RSA_pub_key_new(&ctx, ndata, nlen, edata, elen)`.
     `_libssh2_axtls_rsa_sha1_verify` / `..._sha2_verify` are then
     `RSA_decrypt(ctx, sig, out, sizeof out, 0)` followed by a
     PKCS#1 v1.5 DigestInfo comparison: strip the `00 01 FF..FF 00`
     padding, match the ASN.1 DigestInfo prefix for the hash in
     question, then `memcmp` the digest. Both prefixes are fixed
     byte strings; hard-code them rather than pulling in an ASN.1
     parser.
   - **(b) DH group KEX.** `_libssh2_axtls_dh_key_pair` and
     `_libssh2_axtls_dh_secret` are modular exponentiation over the
     group prime, which axtls's `bi_mod_power` already provides.
     Only needed as a fallback; curve25519 works today.
   - **(c) RSA private keys** (`_libssh2_axtls_rsa_new_private*`).
     Biggest slice and lowest value: it means parsing the OpenSSH
     private key container, which may be bcrypt-KDF encrypted, and
     axtls brings no parser for it. Do this last, and only if
     client-side pubkey auth is actually wanted — the build-time
     inlining recipe in
     [`docs/library/ssh.md`](library/ssh.md) covers that case today.

   **Do not land (a) without an end-to-end test.** A signature
   verifier that returns "valid" on a bad signature is worse than
   one that refuses everything, which is what the stub does now.
   `rigs/ssh-rig/` drives a real paramiko server and is the right
   harness; point it at an RSA host key instead of ed25519. Note
   that rig needs networking under QEMU but not disk, so item 2
   does not block it.

2. **FIXED: disk I/O under QEMU + FreeDOS.** It was PMODE/W,
   plus a stack-alignment bug in our own LE writer. Build with
   DOS/32A and everything works:

       python -m addons.harness.exe build/micropython.asm -o MP.EXE \
           --extender=dos32a --stub-binary <path/to/DOS32A.EXE>

   Verified on QEMU + FreeDOS, DOSBox-X and dosiz with one
   binary: script execution with argv, open/read/create/write/
   append, `os.listdir` / `os.stat` / `os.path.*` /
   `shutil.copyfile`, and containment.

   **What it actually was.** Two independent faults that
   masked each other:

   - **PMODE/W's real-mode call path.** Seen from the QEMU
     monitor while hung: the CPU spins in the BIOS at
     `f000:8913` with `imr=0xFF` — every IRQ masked — and
     `irr` showing IRQ 6 pending. The BIOS disk service is
     waiting for a completion flag its own masked handler can
     never set. At the DOS prompt the mask is a healthy
     `0xB8`, and nothing in this port writes the PIC.
     Unmasking the disk lines from protected mode before the
     call does not help — the machine then halts instead of
     hanging, because the interrupt arrives in a real-mode
     environment the extender has not prepared. This is why
     PMODE/W's own INT 21h translation failed identically.
     DOS/32A does not have the problem.

   - **Misaligned stack under DOS/32A** (upyle, fixed in
     0.1.2). `explicit_stack_object` set `init_esp` to
     `bss_virt_size`, usually odd — `0x187ffb` here — so every
     stack local was misaligned. x86 does not care for ordinary
     loads and stores, which is why a minimal C test passes,
     but MicroPython tags small ints as `(n << 1) | 1` and
     needs object pointers 4-byte aligned. The address of a
     stack-allocated object read back as a small int, and
     `2 in [1, 2, 3]` raised
     `TypeError: 'int' object isn't an iterator` — the iterator
     buffer `py/runtime.c` puts on the stack. `init_esp` is now
     aligned down to 16.

   Note the two hid each other: PMODE/W builds had working
   containment but no disk, DOS/32A builds had disk but broken
   containment. Only fixing both gives a working configuration.

   **Still open, minor:** PMODE/W remains the harness default
   because its stub is bundled, so a plain build still cannot
   do disk I/O on QEMU. DOS/32A needs its stub passed with
   `--stub-binary`. If PMODE/W is not needed, making DOS/32A
   the default (bundling its stub — it is BSD-licensed, so
   redistribution is fine with attribution) would make the
   default build work everywhere.

3. **Public-key auth from file** —
   `session.userauth_publickey_fromfile(user, pub_path,
   priv_path, passphrase)`. Still unimplemented, but no longer
   blocked on a mystery: file reading works wherever disk I/O
   works (verified under DOSBox-X), so this is now ordinary
   work — either bind the libssh2 call, or add a Python-side
   helper that reads the file with MicroPython `open()` and
   calls the existing `userauth_publickey(user, privkey_bytes)`.
   On QEMU + FreeDOS it will inherit the item-2 disk wedge.

4. **Document where SSH credentials live on DOS.** There's no
   `~/.ssh/` on DOS, so the conventions need to be spelled out:
   where MP looks for `id_ed25519` / `known_hosts` /
   `authorized_keys` (likely a single `\SSH\` dir at the root
   of whatever drive the user runs from? or current-dir
   fallback?), how to point `examples/sftp.py` /
   `examples/scp.py` at a non-default location, and the 8.3
   filename mapping (e.g. `id_ed25519` won't fit — pick
   `ID_ED.KEY` or similar and document it). Also: pubkey-auth
   from a file currently can't be loaded at runtime (see #2/#3
   above), so document the build-time inlining pattern as the
   interim recipe until that's fixed.

   Written up in [`docs/library/ssh.md`](library/ssh.md) under
   "Where credentials live on DOS".

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
