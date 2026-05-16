# WIP — SSH/SCP under freedos_micro_python

Snapshot at HEAD `7ab2359` (freedos) + `73b99df` (uc386). All changes
committed; nothing in the working tree.

## What works

- **libssh2 1.11.1 + TweetNaCl** fetched into `upstream/lib/{libssh2,tweetnacl}/`.
- **Crypto adapter** (`port/libssh2_axtls.{h,c}`) maps libssh2's `crypto.h`
  API to axtls (SHA1/256/384/512, MD5, HMAC, AES-CBC, AES-CTR) + TweetNaCl
  (Curve25519 KEX, Ed25519 verify). RSA / DH / key-parse paths are stubs
  returning `-1`.
- **All 25 libssh2 .c files compile clean** through uc386 (verified by build
  #13 onward).
- **`_ssh` MicroPython module** (`port/modssh_uc386dos.c`) operational —
  smoke test `test_micropython_import_ssh` PASSES under dos_emu:
  ```
  >>> import _ssh
  >>> _ssh.version()           → '1.11.1'
  >>> _ssh.crypto_engine_name() → 'axtls'
  ```
- **MP.EXE** at `rigs/dosbox-x-rig/MP.EXE` (526,097 bytes, +170 over
  pre-SSH baseline after DCE).
- **micropython.bin** at `/private/tmp/fdmp-build/build/micropython.bin`
  (452,926 bytes).

## What's broken

- **TLS rig regression** (`rigs/tls-rig/run-tls-rig.sh`). TLSTEST.PY hangs at
  `ctx_ready` (wrap_socket). Pcap shows Client Hello + Server cert response —
  but on current builds, **client never ACKs the 915-byte response**. Server
  retransmits 5+ times with exponential backoff, then gives up.
  - Environmental ruled out 2026-05-16: host OpenSSL 3.6.2 + `tls_server.py`
    handshakes cleanly against `openssl s_client -tls1 -cipher
    'AES128-SHA:@SECLEVEL=0'`. TLSv1.0 still works.
  - libssh2 source ruled out (earlier WIP entry): byte-identical binary
    without libssh2 sources still hangs.
  - DPMI 0x0303 callback (commit `7f23f1c`) ruled out 2026-05-16: patched
    that line out, rebuilt, same hang.
  - rmstub memcpy race ruled out 2026-05-16: masked NIC IRQs around
    `pktdrv_poll_rmstub`'s memcpy + `*st_pending = 0` store, same hang.
  - uc386 codegen `73b99df` (compll-dedup) ruled out 2026-05-16:
    checked out `src/uc386/codegen.py` to `73b99df~1` and rebuilt;
    same hang. (Restored after the test; uc386 tree clean.)
  - **Diagnostic narrowing (2026-05-16):** added markers `[ep:NNNN]` in
    `uc386dos_eth_pump_rx` per packet, `[trEnter]`/`[trWait]`/`[trL]`/`[trCb]`
    in `lwip_tcp_receive` + `_lwip_tcp_recv`, `[hsE]`..`[Fi+]` in axtls
    `do_clnt_handshake` via `patch_axtls_handshake_markers` in fetch.sh.
    Markers show:
    - Small packets (60, 64 byte) reach `uc386dos_eth_pump_rx` and lwIP
      accepts them (`[ep:iO]`).
    - The 973-byte server response is **never seen by `uc386dos_eth_pump_rx`**
      — but NE2000 `BNRY` register advances by 4 pages each retransmit
      (Crynwr consumed it from the NIC ring).
    - `_lwip_tcp_recv` (`[trCb]`) is never called.
    - `do_clnt_handshake` (`[hsE]`) is never reached.
    - Conclusion: the rmstub at `thunk_seg:0x10` is dropping the 973-byte
      packets at phase 0 (returning `ES:DI = 0:0`). Crynwr discards them
      and advances BNRY anyway. Single-slot pending=1 race is the
      strongest remaining hypothesis but IRQ-masking the drain didn't
      fix it.
  - Next ideas worth trying (each costs ~25-min rebuild):
    a. Revert uc386 `73b99df codegen: _compound_assign_ll reuses existing
       __compll_* slots` — could be miscompiling something in the RX path
       (untested).
    b. Revive 51b52e1's 2-slot RX ring; revisit the e83c45b
       `[ps:in]`→`[ps:post-dpmi]` send hang since intervening commits
       may have fixed it.
    c. Add diagnostic markers in `pktdrv_poll_rmstub` for st_pending /
       st_length and in Crynwr's phase 0 path (real-mode asm — would
       need a fixed real-mode print routine).
    d. Bisect from `e83c45b` (known-passing per its commit message) to
       HEAD across uc386 + freedos repos.
  - Current diagnostic state in working tree (uncommitted as of
    2026-05-16): `port/lwip_uc386dos.c` has `[ep:NNNN]`/`[ep:iO]`/`[ep:iE]`
    markers; `scripts/fetch.sh` has `patch_axtls_handshake_markers`
    function that injects `[hsE]`..`[Fi+]` markers in axtls
    `do_clnt_handshake`. Markers can stay (re-applied on every fetch via
    fetch.sh) — they don't affect correctness, only add ~30 bytes per
    received packet of COM1 output.
  - **wget rig** at `rigs/tls-rig/run-wget-rig.sh` hits the same hang (it
    uses the same axtls TLS path).

## Pick-up points

In rough order of effort/value:

1. **Fix tweetnacl `crypto_scalarmult` arithmetic bug** —
   2026-05-16 session got SSH all the way to ed25519 verify
   running end-to-end (commits `be1985e` → `168d6ed`). The
   remaining gap: client and server compute *different* shared
   secret K values:

       Client K[0:8] = 585dc06000000000
       Server K[0:8] = fc886b78b052c0c6

   Bytes 4–7 of our K being all-zero is the tell. `crypto_scalarmult`
   works when `p` is `_9` (the base point — used by
   `crypto_scalarmult_base` for our public key, which the server
   accepts) but produces a wrong result when `p` is a real X25519
   public key from the server. Most likely a uc386 codegen bug
   in `M()` or `car25519()` that only manifests when both
   multiply operands are non-zero (when `p`=`_9`, `b[1..15]=0` so
   most inner-loop multiplications reduce to `a[i]*0`).

   Concrete next steps:
   - Write a stand-alone X25519 test vector check (RFC 7748:
     priv `a546...`, pub `e6db...` should give out `c3da...`).
     If the test fails on uc386, isolate the smallest input that
     causes divergence.
   - Instrument `M()` to dump intermediate `t[i]` values.
   - Try rewriting compound assigns inside `M()` / `car25519()`
     as plain stores (same workaround as the `pack25519` fix in
     `patch_tweetnacl_uc386dos`).

   Diagnostic infrastructure already in tree:
   - `[K:NNNN]` marker in `_libssh2_axtls_curve25519_gen_k` dumps
     first 8 bytes of K.
   - `ssh_server.py` monkey-patches paramiko's
     `KexCurve25519._perform_exchange` to log the same bytes.

   Stack-frame and `pack25519` codegen issues are fixed
   (commit `0f51af1`, formalized in
   `scripts/fetch.sh:patch_tweetnacl_uc386dos`).

   Operational notes:
   - Rig at `rigs/ssh-rig/run-ssh-rig.sh` — paramiko Ed25519 host
     key + password auth (testuser / testpass).
   - To resume: `freedos-micropython --workdir /private/tmp/fdmp-build
     port` (the build tree gets wiped on tmp clean; everything is
     re-fetched cleanly via `scripts/fetch.sh`).
   - **Do not put back `-DPKTDRV_FORCE_CRYNWR=1`** in
     `scripts/build_port.sh` for the SSH path — Crynwr + the rmstub
     bounce-buffer drop the 920-byte server-banner+KEXINIT packet.
     PM-native NE2000 (FORCE_CRYNWR=0) is what makes the KEX wire
     path go through.

2. **Restore TLS rig** — previously regressed (see "What's broken"
   above).  May or may not share root cause with the SSH
   hash-verify gap.  Try with PM-native NE2000 (drop FORCE_CRYNWR
   from build_port.sh — already done as of `be1985e`) and check
   if the TLS rig too completes further; the rmstub drop bug at
   the rmstub level was the dominant blocker.

3. **Real RSA/DH/key-parse implementations** in
   `port/libssh2_axtls.c`. Lines ~440–620; bn_* and curve25519_gen_k
   are now real (commit `be1985e`), RSA/DH still stubs:
   - `_libssh2_axtls_rsa_new` — wrap raw n/e/d into `RSA_CTX` via
     `RSA_pub_key_new` / `RSA_priv_key_new`.
   - `_libssh2_axtls_rsa_sha{1,2}_sign` — `RSA_encrypt` with priv key
     + PKCS#1 v1.5 padding.
   - `_libssh2_axtls_rsa_sha{1,2}_verify` — call axtls's `RSA_decrypt`
     with `is_decryption=0` + DER prefix match (SHA1/256/384/512).
   - `_libssh2_axtls_pub_priv_keyfile{,memory}` — PEM parser; axtls's
     `loader.c` already does PKCS#1 / PKCS#8.
   - `_libssh2_axtls_dh_*` — wrap axtls's bigint API for
     diffie-hellman-group* KEX fallback when curve25519 isn't
     negotiated.  Optional — modssh restricts KEX to curve25519
     so this is only needed for interop with old SSH servers.

4. **Expand `_ssh` MP API** with SFTP wrapper in
   `port/modssh_uc386dos.c`.  Session / userauth_password / exec /
   close already landed (commit `be1985e`); SFTP not yet:
   - `session.sftp()` → SFTP object
   - `sftp.open(path, mode)`, `sftp.read/write/close`

5. **Python wrappers** at `examples/{sftp,scp}.py` — paramiko-shaped
   interface on top of `_ssh`. Same shape as `examples/wget.py`.
   Model: copy `run-wget-rig.sh`, swap the tls_server.py for sshd
   (via openssh's `/usr/sbin/sshd -p PORT -h KEYFILE -D`). Test
   uploads + downloads round-trip a known marker.

## Commits this session (oldest → newest)

**freedos_micro_python** (`git log --oneline 681ddb8..HEAD`):
```
6c08104 fetch: pull libssh2 + TweetNaCl for the SSH/SCP work
2e00008 libssh2: skeleton crypto backend on axtls + TweetNaCl
01b133c fetch: patch libssh2 callback-macro continuation lines for uc386
4cac5c4 libssh2 axtls: opaque ctx storage to dodge `comp` typedef collision
2c6c6e5 fetch: extend callback-macro join patch to libssh2 src/ files
9fa7b4f build: skip chacha/blowfish files + patch BSD u_int/u_char typedefs
c13eb92 fetch: extend BSD-types patch to libssh2 poly1305.h
2d0894d fetch: BSD-types patch also covers crypt.c
acc52ab build: define HAVE_SELECT + HAVE_SYS_SELECT_H for libssh2
70934fd build: define HAVE_SYS_UIO_H so libssh2 picks up iovec typedef
0e4d66b libssh2 axtls: add LIBSSH2_DH_MAX_MODULUS_BITS define
2c44bd6 libssh2 axtls: add LIBSSH2_DH_GEX_{MIN,OPT,MAX}GROUP constants
256f60b fetch: hoist libssh2 sftp.h handle_type anonymous enum to file scope
421980f fetch: add libssh2_axtls to libssh2.h crypto_engine_t enum
ebce5c7 libssh2 axtls: declare cipher IDs for chacha20/arcfour/cast/aes-gcm
0d04980 build_port: tighten comment on libssh2 source list
bc04ae2 fetch: use 32-bit golden-ratio for send_client_hello counter
cc2158d _ssh module: skeleton MP wrapper around libssh2
7ab2359 test_smoke: pin _ssh module + libssh2 version + axtls backend
```

**uc386** (`git log --oneline 1c35184..HEAD`):
```
1a12a39 codegen: scope-aware stack reuse for block-scope locals
7ec4eed libc: add sys/select.h shim — fd_set, FD_*, select() prototype
c798478 libc: add sys/uio.h shim — struct iovec + readv/writev prototypes
73b99df codegen: _compound_assign_ll reuses existing __compll_* slots
```

## Build commands

```sh
# Fetch + rebuild MP.EXE
cd /private/tmp/fdmp-build  # or wherever the build tree lives
PYTHON=/Users/wohl/src/uc386/.venv/bin/python \
UC386_LIB_INCLUDE=/Users/wohl/src/uc386/src/uc386/lib/include \
  ~/src/freedos_micro_python/src/freedos_micro_python/scripts/fetch.sh
PYTHON=/Users/wohl/src/uc386/.venv/bin/python \
UC386_LIB_INCLUDE=/Users/wohl/src/uc386/src/uc386/lib/include \
  ~/src/freedos_micro_python/src/freedos_micro_python/scripts/build_port.sh
# ~25 min; produces build/micropython.bin

# Link MP.EXE
cd ~/src/uc386
.venv/bin/python -m addons.harness.exe \
  /private/tmp/fdmp-build/build/micropython.asm \
  -o ~/src/freedos_micro_python/rigs/dosbox-x-rig/MP.EXE

# Smoke test under dos_emu (fast, no qemu)
cd ~/src/freedos_micro_python
FREEDOS_MP_BIN=/private/tmp/fdmp-build/build/micropython.bin \
  ~/src/uc386/.venv/bin/python -m pytest \
  tests/test_smoke.py::test_micropython_import_ssh -v

# qemu+FreeDOS TLS rig (currently REGRESSED — hangs)
cd ~/src/freedos_micro_python/rigs/tls-rig
./run-tls-rig.sh
```
