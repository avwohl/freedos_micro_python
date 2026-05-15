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
  `ctx_ready` (wrap_socket). Pcap shows Client Hello + Server cert response +
  ACK on the wire — hang is inside MP/axtls's post-cert handling.
  - Bisected to NOT be caused by libssh2 source inclusion: build_port.sh with
    libssh2 sources commented out produces a byte-identical 452,492-byte
    micropython.bin and the rig still hangs.
  - Cause unidentified; likely a subtle codegen change in some axtls
    function from the uc386 scope-frame fix or the compll-dedup fix, OR
    environmental (host OpenSSL 3.6.2 dropping TLSv1.0 support, qemu state,
    etc.).
  - **wget rig** at `rigs/tls-rig/run-wget-rig.sh` hits the same hang (it
    uses the same axtls TLS path).

## Pick-up points

In rough order of effort/value:

1. **Fix TLS regression** — needs hands-on binary diff (we don't have a
   passing binary saved). Useful approach: instrument
   `axtls/ssl/tls1.c:do_clnt_handshake` post-`HS_CERTIFICATE` with `write(1,
   "[hs:N]", N)` markers via fetch.sh patch and rebuild — narrows the hang
   to a specific axtls call within ~3 build cycles.

2. **Real RSA/DH/key-parse implementations** in
   `port/libssh2_axtls.c` (currently all stubs at lines ~280–470). Most
   ones needed:
   - `_libssh2_axtls_rsa_new` — wrap raw n/e/d into `RSA_CTX` via
     `RSA_pub_key_new` / `RSA_priv_key_new`.
   - `_libssh2_axtls_rsa_sha{1,2}_sign` — `RSA_encrypt` with priv key
     + PKCS#1 v1.5 padding.
   - `_libssh2_axtls_rsa_sha{1,2}_verify` — call axtls's `RSA_verify`.
   - `_libssh2_axtls_pub_priv_keyfile{,memory}` — PEM parser; axtls's
     `loader.c` already does PKCS#1 / PKCS#8.
   - `_libssh2_axtls_dh_*` and `_libssh2_axtls_bn_*` — wrap axtls's
     bigint API.

3. **Expand `_ssh` MP API** with Session/Channel/SFTP wrappers in
   `port/modssh_uc386dos.c`. Pattern lives in
   `port/modtls_axtls_uc386dos.c` (SSLContext + SSLSocket types).
   Minimum useful surface:
   - `_ssh.Session(socket)` → does handshake on a connected socket
   - `session.userauth_password(user, pw)`
   - `session.sftp()` → SFTP object
   - `sftp.open(path, mode)`, `sftp.read/write/close`

4. **Python wrappers** at `examples/{sftp,scp}.py` — paramiko-shaped
   interface on top of `_ssh`. Same shape as `examples/wget.py`.

5. **`run-ssh-rig.sh`** — host-side OpenSSH + qemu+FreeDOS MP rig.
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
