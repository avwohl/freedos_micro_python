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
import socket
import sys
import threading
import time
from pathlib import Path

import paramiko
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
# exchange hash by intercepting hash_algo on the curve25519 kex.
import hashlib
_orig_h = _kc25.KexCurve25519
_orig_parse_init = _orig_h._parse_kexecdh_init
def _dump_parse_init(self, m):
    # Wrap the hash algo so we capture the input bytes.
    orig_algo = self.hash_algo
    def _wrap(data=b""):
        h = orig_algo(data)
        print(f"[ssh-server] hash_input_len={len(data)} "
              f"first48={data[:48].hex()} "
              f"last16={data[-16:].hex()}",
              flush=True)
        return h
    self.hash_algo = _wrap
    try:
        return _orig_parse_init(self, m)
    finally:
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


def serve_one(port: int, host_key_path: Path, max_seconds: float) -> int:
    host_key = paramiko.Ed25519Key(filename=str(host_key_path))

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

    # Wait for the exec request to land.
    if not server.event.wait(timeout=max_seconds):
        print("[ssh-server] timed out waiting for exec", flush=True)
        return 4
    print(f"[ssh-server] exec command: {server.command!r}", flush=True)

    channel.sendall(b"SSH_RIG_OK\n")
    channel.send_exit_status(0)
    channel.close()
    transport.close()
    print("[ssh-server] sent marker; closed", flush=True)
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
