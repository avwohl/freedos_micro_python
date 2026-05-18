"""Minimal scp replacement for MicroPython under uc386-dos.

Built against MP's `socket` (lwIP-backed) and `_ssh` (libssh2-backed,
axtls crypto). Wraps libssh2's `scp_recv2` / `scp_send_ex` through the
port's `session.scp_recv()` / `session.scp_send()` API.

Usage from the REPL or `MP.EXE SCP.PY`:

    import scp
    scp.main(["host:/remote/path", "local.txt"])              # download
    scp.main(["local.txt", "host:/remote/path"])              # upload

Flags:
    -P <port>        SSH port (default 22)
    -u <user>        login user (default "testuser")
    -p <password>    password (default "testpass")
    -h, --help

Only password auth is wired up today (public-key auth requires
key parsing in `port/libssh2_axtls.c` which is still stubbed).
The first arg containing `:` is treated as the remote; the other
arg is the local path. Direction (upload vs download) is inferred
from which side has the colon.
"""

import sys
import socket
import _ssh


def _split_remote(spec):
    # "host:/path" → ("host", "/path"); "user@host:/path" → already
    # stripped to ("host", "/path") because user is handled separately.
    host, _, path = spec.partition(":")
    if "@" in host:
        host = host.partition("@")[2]
    return host, path


def _connect(host, port, user, password):
    addr = socket.getaddrinfo(host, port)[0][-1]
    s = socket.socket()
    s.connect(addr)
    s.settimeout(0.1)
    sess = _ssh.Session(s)
    sess.userauth_password(user, password)
    return sess


def fetch(remote_spec, out_path, port=22, user="testuser", password="testpass"):
    host, remote = _split_remote(remote_spec)
    sess = _connect(host, port, user, password)
    try:
        data = sess.scp_recv(remote)
        with open(out_path, "wb") as f:
            f.write(data)
        return len(data)
    finally:
        sess.close()


def put(local_path, remote_spec, port=22, user="testuser", password="testpass",
        mode=0o644):
    host, remote = _split_remote(remote_spec)
    with open(local_path, "rb") as f:
        data = f.read()
    sess = _connect(host, port, user, password)
    try:
        n = sess.scp_send(remote, mode, data)
        return n
    finally:
        sess.close()


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    port = 22
    user = "testuser"
    password = "testpass"
    positional = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "-P" and i + 1 < len(argv):
            port = int(argv[i + 1]); i += 2
        elif a == "-u" and i + 1 < len(argv):
            user = argv[i + 1]; i += 2
        elif a == "-p" and i + 1 < len(argv):
            password = argv[i + 1]; i += 2
        elif a in ("-h", "--help"):
            print("usage: scp.py [-P PORT] [-u USER] [-p PASS] SRC DST")
            print("       SRC or DST is host:/path; the other is a local file.")
            return 0
        elif a.startswith("-"):
            sys.stderr.write("unknown flag: " + a + "\n"); return 2
        else:
            positional.append(a); i += 1
    if len(positional) != 2:
        sys.stderr.write("usage: scp.py [-P PORT] [-u USER] [-p PASS] SRC DST\n")
        return 2
    src, dst = positional
    src_remote = ":" in src
    dst_remote = ":" in dst
    try:
        if src_remote and not dst_remote:
            n = fetch(src, dst, port=port, user=user, password=password)
            print("got " + str(n) + " bytes from " + src + " to " + dst)
        elif dst_remote and not src_remote:
            n = put(src, dst, port=port, user=user, password=password)
            print("sent " + str(n) + " bytes from " + src + " to " + dst)
        else:
            sys.stderr.write("scp: exactly one of SRC/DST must be host:/path\n")
            return 2
    except OSError as e:
        sys.stderr.write("scp: " + str(e) + "\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
