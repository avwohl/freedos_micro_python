"""Minimal sftp replacement for MicroPython under uc386-dos.

Built against MP's `socket` (lwIP-backed) and `_ssh` (libssh2-backed,
axtls crypto). Wraps libssh2's SFTP subsystem through the port's
`session.sftp()` / `SFTP.open()` / `SFTPFile.read/write/close` API.

Usage from the REPL or `MP.EXE SFTP.PY`:

    import sftp
    sftp.main(["get", "host:/remote/path", "local.txt"])
    sftp.main(["put", "local.txt", "host:/remote/path"])

Flags:
    -P <port>        SSH port (default 22)
    -u <user>        login user (default "testuser")
    -p <password>    password (default "testpass")
    -h, --help

Subcommands `get` and `put` parallel scp.py; SFTP differs in that
it can handle larger files by streaming through `read(n)` / `write(buf)`
chunks rather than holding the whole payload in RAM. Today we still
buffer the full file (the test scenarios are small); revisit when
real workloads need it.
"""

import sys
import socket
import _ssh


def _split_remote(spec):
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


def get(remote_spec, out_path, port=22, user="testuser", password="testpass"):
    host, remote = _split_remote(remote_spec)
    sess = _connect(host, port, user, password)
    try:
        sftp = sess.sftp()
        try:
            f = sftp.open(remote, "r")
            try:
                data = f.read()
            finally:
                f.close()
            with open(out_path, "wb") as out:
                out.write(data)
            return len(data)
        finally:
            sftp.close()
    finally:
        sess.close()


def put(local_path, remote_spec, port=22, user="testuser", password="testpass"):
    host, remote = _split_remote(remote_spec)
    with open(local_path, "rb") as f:
        data = f.read()
    sess = _connect(host, port, user, password)
    try:
        sftp = sess.sftp()
        try:
            f = sftp.open(remote, "w")
            try:
                n = f.write(data)
            finally:
                f.close()
            return n
        finally:
            sftp.close()
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
            print("usage: sftp.py [-P PORT] [-u USER] [-p PASS] get|put SRC DST")
            return 0
        elif a.startswith("-"):
            sys.stderr.write("unknown flag: " + a + "\n"); return 2
        else:
            positional.append(a); i += 1
    if len(positional) != 3 or positional[0] not in ("get", "put"):
        sys.stderr.write("usage: sftp.py [-P PORT] [-u USER] [-p PASS] get|put SRC DST\n")
        return 2
    cmd, src, dst = positional
    try:
        if cmd == "get":
            n = get(src, dst, port=port, user=user, password=password)
            print("got " + str(n) + " bytes from " + src + " to " + dst)
        else:
            n = put(src, dst, port=port, user=user, password=password)
            print("sent " + str(n) + " bytes from " + src + " to " + dst)
    except OSError as e:
        sys.stderr.write("sftp: " + str(e) + "\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
