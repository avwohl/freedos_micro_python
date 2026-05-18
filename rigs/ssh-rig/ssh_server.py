#!/usr/bin/env python3
"""Tiny paramiko-based SSH server for the uc386 MP ssh rig.

Listens on `--port`, accepts one connection, handshakes with an
Ed25519 host key, accepts `testuser` + `testpass` via password
auth, and responds to any exec request by writing a fixed
`SSH_RIG_OK\\n` marker on the channel then closing.

The Ed25519 host key file is generated on first run via ssh-keygen
in the rig script (paramiko's Ed25519Key.generate isn't in
paramiko 4.x's public API). The pubkey can also be read out for
the client's known_hosts pinning, but the rig uses
LIBSSH2_CALLBACK_HOSTKEY-ignore at present — host key validation
is a separate slice of work.
"""

import argparse
import logging
import socket
import sys
import threading
import time
from pathlib import Path

import paramiko

# Verbose paramiko logging so we can see post-handshake packet flow.
logging.basicConfig(level=logging.DEBUG, format="[paramiko] %(message)s")
paramiko.util.log_to_file("/tmp/paramiko-server.log", level="DEBUG")

# paramiko's `Exception in subsystem handler` log just `format`s the
# exception, which often yields an empty string. Patch start_subsystem
# to dump the traceback to stderr so we can see WHAT failed.
import traceback
_orig_start_sub = paramiko.SubsystemHandler.start_subsystem
def _patched_start_sub(self, *a, **kw):
    try:
        return _orig_start_sub(self, *a, **kw)
    except BaseException:
        print("[paramiko-traceback]", traceback.format_exc(), flush=True)
        raise
paramiko.SubsystemHandler.start_subsystem = _patched_start_sub

# Tap raw socket recv to see if encrypted SERVICE_REQUEST actually arrives.
import paramiko.packet as _ppkt
_orig_read_all = _ppkt.Packetizer.read_all
def _tap_read_all(self, n, check_rekey=False):
    data = _orig_read_all(self, n, check_rekey)
    print(f"[server-recv] {len(data)}B {data[:32].hex() if data else ''}",
          flush=True)
    return data
_ppkt.Packetizer.read_all = _tap_read_all

_orig_send_message = _ppkt.Packetizer.send_message
def _tap_send_message(self, data):
    body = bytes(data.asbytes()) if hasattr(data, "asbytes") else bytes(data)
    ptype = body[0] if body else -1
    print(f"[server-send] type={ptype} body={body[:48].hex()}", flush=True)
    return _orig_send_message(self, data)
_ppkt.Packetizer.send_message = _tap_send_message

# Tap read_message to dump the decrypted packet so we can compare
# server's view of our encrypted bytes against the plaintext we
# expected. Catch the MAC/blocking exceptions too so we can see them.
_orig_read_message = _ppkt.Packetizer.read_message
def _tap_read_message(self):
    try:
        rc = _orig_read_message(self)
        ptype, m = rc
        body = m.get_remainder()
        m.rewind()
        print(f"[server-msg] type={ptype} body={body[:48].hex()}", flush=True)
        return rc
    except Exception as e:
        print(f"[server-msg] EXCEPTION {type(e).__name__}: {e}", flush=True)
        raise
_ppkt.Packetizer.read_message = _tap_read_message
import paramiko.kex_curve25519 as _kc25
_orig_exchange = _kc25.KexCurve25519._perform_exchange
def _patched_exchange(self, peer_key):
    secret = _orig_exchange(self, peer_key)
    print(f"[ssh-server] K[0:8]={secret[:8].hex()}", flush=True)
    return secret
_kc25.KexCurve25519._perform_exchange = _patched_exchange

# Also intercept the exchange hash H by monkey-patching the
# transport's _set_K_H call (paramiko stores K + H there).
import paramiko.transport as _pt
_orig_set_K_H = _pt.Transport._set_K_H
def _patched_set_K_H(self, K, H):
    print(f"[ssh-server] H[0:8]={H[:8].hex()}", flush=True)
    return _orig_set_K_H(self, K, H)
_pt.Transport._set_K_H = _patched_set_K_H

# Dump the FULL bytes paramiko hashes (hm.asbytes()) for the
# exchange hash, plus per-field hex bytes, by intercepting Message.add
# during the curve25519 kex parse-init.
import hashlib
_orig_h = _kc25.KexCurve25519
_orig_parse_init = _orig_h._parse_kexecdh_init
def _dump_parse_init(self, m):
    import paramiko.message as _pmsg
    from paramiko.util import asbytes
    _real_add_string = _pmsg.Message.add_string
    _field_idx = [0]
    def _wrap_add_string(self2, s):
        b = asbytes(s)
        print(f"[ssh-server] field{_field_idx[0]} len={len(b)} "
              f"sha={hashlib.sha256(b).hexdigest()[:16]} "
              f"first8={b[:8].hex()} mid8={b[len(b)//2:len(b)//2+8].hex()} "
              f"last8={b[-8:].hex()}", flush=True)
        _field_idx[0] += 1
        return _real_add_string(self2, s)
    _pmsg.Message.add_string = _wrap_add_string
    # And wrap hash_algo to dump total len.
    orig_algo = self.hash_algo
    def _wrap(data=b""):
        h = orig_algo(data)
        print(f"[ssh-server] hash_input_len={len(data)} "
              f"first48={data[:48].hex()} last16={data[-16:].hex()}",
              flush=True)
        return h
    self.hash_algo = _wrap
    try:
        return _orig_parse_init(self, m)
    finally:
        _pmsg.Message.add_string = _real_add_string
        self.hash_algo = orig_algo
_orig_h._parse_kexecdh_init = _dump_parse_init


HERE = Path(__file__).resolve().parent


class Server(paramiko.ServerInterface):
    def __init__(self):
        self.event = threading.Event()
        self.command = None

    def check_auth_password(self, username, password):
        if username == "testuser" and password == "testpass":
            return paramiko.AUTH_SUCCESSFUL
        return paramiko.AUTH_FAILED

    def get_allowed_auths(self, username):
        return "password"

    def check_channel_request(self, kind, chanid):
        if kind == "session":
            return paramiko.OPEN_SUCCEEDED
        return paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

    def check_channel_exec_request(self, channel, command):
        # Stash and signal the main thread.
        self.command = command.decode("utf-8", "replace") \
            if isinstance(command, bytes) else command
        self.event.set()
        return True


class _InMemorySFTPServer(paramiko.SFTPServerInterface):
    """In-RAM SFTP backend so the rig has full control over what the
    client reads / writes. `files` is a {path: bytes} dict the rig
    seeds before the client connects, and the rig inspects after."""

    files = {}  # populated by serve_one() before transport.start_server

    def open(self, path, flags, attr):
        import os as _os
        if flags & _os.O_WRONLY or flags & _os.O_RDWR:
            # Write/create — start fresh if O_TRUNC, else preserve.
            if flags & _os.O_TRUNC:
                _InMemorySFTPServer.files[path] = b""
            elif path not in _InMemorySFTPServer.files:
                _InMemorySFTPServer.files[path] = b""
        else:
            # Read — must already exist.
            if path not in _InMemorySFTPServer.files:
                return paramiko.SFTP_NO_SUCH_FILE
        h = paramiko.SFTPHandle(flags)
        h._rig_path = path
        return h


def _sftp_handle_read(self, offset, length):
    data = _InMemorySFTPServer.files.get(self._rig_path, b"")
    return data[offset:offset + length]


def _sftp_handle_write(self, offset, data):
    cur = _InMemorySFTPServer.files.get(self._rig_path, b"")
    cur = cur.ljust(offset, b"\x00") + data + cur[offset + len(data):]
    _InMemorySFTPServer.files[self._rig_path] = cur
    return paramiko.SFTP_OK


def _sftp_handle_close(self):
    return paramiko.SFTP_OK


# Patch SFTPHandle once at import time (avoids per-handle subclassing).
paramiko.SFTPHandle.read = _sftp_handle_read
paramiko.SFTPHandle.write = _sftp_handle_write
paramiko.SFTPHandle.close = _sftp_handle_close


def serve_one(port: int, host_key_path: Path, max_seconds: float) -> int:
    host_key = paramiko.Ed25519Key(filename=str(host_key_path))

    # Seed an SFTP file the rig client will fetch + verify, and pre-
    # create the path the client will upload to.
    _InMemorySFTPServer.files["/sftp/download.txt"] = b"SFTP_RIG_OK_DL\n"
    _InMemorySFTPServer.files["/sftp/upload.txt"] = b""

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    sock.listen(1)
    sock.settimeout(max_seconds)
    print(f"[ssh-server] listening on 0.0.0.0:{port}", flush=True)

    try:
        client, addr = sock.accept()
    except socket.timeout:
        print("[ssh-server] timeout waiting for connection", flush=True)
        return 1
    print(f"[ssh-server] connection from {addr}", flush=True)

    transport = paramiko.Transport(client)
    transport.add_server_key(host_key)
    # Register the SFTP subsystem so sess.sftp() finds something
    # behind the "sftp" channel-subsystem request.
    transport.set_subsystem_handler("sftp", paramiko.SFTPServer,
                                     _InMemorySFTPServer)
    server = Server()
    try:
        transport.start_server(server=server)
    except paramiko.SSHException as e:
        print(f"[ssh-server] handshake failed: {e}", flush=True)
        return 2
    print(f"[ssh-server] handshake ok; cipher={transport.local_cipher}", flush=True)

    channel = transport.accept(timeout=max_seconds)
    if channel is None:
        print("[ssh-server] no channel opened", flush=True)
        return 3
    print("[ssh-server] channel opened", flush=True)

    # Wait briefly for an exec request. SFTP-only tests won't fire
    # an exec — let those proceed to the accept() below.
    if server.event.wait(timeout=2.0):
        print(f"[ssh-server] exec command: {server.command!r}", flush=True)
        channel.sendall(b"SSH_RIG_OK\n")
        channel.send_exit_status(0)
        channel.close()
        print("[ssh-server] sent exec marker", flush=True)
    else:
        print("[ssh-server] no exec request; skipping marker", flush=True)

    # Keep the transport alive until the client disconnects (or the
    # rig hits its own outer timeout). SFTP is wired via
    # set_subsystem_handler and runs in a background thread spawned
    # by paramiko's transport — we don't need to accept() a new
    # channel here. Polling transport.is_active() avoids tearing the
    # connection down while the client is mid-SFTP.
    while transport.is_active():
        time.sleep(0.5)
    print("[ssh-server] transport closed; "
          f"sftp files seen: {sorted(_InMemorySFTPServer.files.items())}",
          flush=True)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=2222)
    ap.add_argument("--host-key", type=Path, default=HERE / "ssh_host_ed25519")
    ap.add_argument("--max-seconds", type=float, default=60.0)
    args = ap.parse_args()
    return serve_one(args.port, args.host_key, args.max_seconds)


if __name__ == "__main__":
    sys.exit(main())
