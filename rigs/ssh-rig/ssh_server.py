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

# Populated by serve_one() from ssh_client_ed25519.pub before
# transport.start_server hands control to Server.check_auth_publickey.
CLIENT_PUBKEY = None


class Server(paramiko.ServerInterface):
    def __init__(self, files):
        self.event = threading.Event()    # set on first exec, kept for back-compat
        self.command = None               # last exec command (for logging)
        self.files = files
        self.exec_count = 0
        self.scp_count = 0

    def check_auth_password(self, username, password):
        if username == "testuser" and password == "testpass":
            return paramiko.AUTH_SUCCESSFUL
        return paramiko.AUTH_FAILED

    def check_auth_publickey(self, username, key):
        # CLIENT_PUBKEY is loaded from ssh_client_ed25519.pub by
        # serve_one() at startup. We accept any user as long as the
        # offered key matches.
        if CLIENT_PUBKEY is not None and key == CLIENT_PUBKEY:
            print(f"[ssh-server] pubkey auth ok for {username!r}",
                  flush=True)
            return paramiko.AUTH_SUCCESSFUL
        print(f"[ssh-server] pubkey auth FAILED for {username!r}",
              flush=True)
        return paramiko.AUTH_FAILED

    def get_allowed_auths(self, username):
        return "password,publickey"

    def check_channel_request(self, kind, chanid):
        if kind == "session":
            return paramiko.OPEN_SUCCEEDED
        return paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

    def check_channel_exec_request(self, channel, command):
        cmd = command.decode("utf-8", "replace") \
            if isinstance(command, bytes) else command
        self.command = cmd
        self.exec_count += 1
        print(f"[ssh-server] exec request #{self.exec_count}: {cmd!r}",
              flush=True)
        # Dispatch in a daemon thread so this callback returns promptly
        # and paramiko's transport loop can keep processing.
        t = threading.Thread(target=_handle_exec,
                              args=(channel, cmd, self),
                              daemon=True)
        t.start()
        self.event.set()
        return True


def _handle_exec(channel, cmd, server):
    """Run on a daemon thread per exec channel. Handles either an SCP
    command (delegates to _scp_serve) or a tiny `echo TOKEN`
    impl that lets the rig differentiate the password vs publickey
    auth paths by what they ask for."""
    try:
        if cmd.startswith("scp "):
            ok = _scp_serve(channel, cmd, server.files)
            server.scp_count += 1
            print(f"[ssh-server] scp #{server.scp_count} "
                  f"{'ok' if ok else 'FAIL'}: {cmd!r}", flush=True)
            channel.send_exit_status(0 if ok else 1)
        elif cmd.startswith("echo "):
            arg = cmd[len("echo "):]
            payload = (arg + "\n").encode("utf-8")
            channel.sendall(payload)
            channel.send_exit_status(0)
            print(f"[ssh-server] echo'd {arg!r} for {cmd!r}", flush=True)
        else:
            channel.sendall(b"SSH_RIG_OK\n")
            channel.send_exit_status(0)
            print(f"[ssh-server] sent default marker for {cmd!r}", flush=True)
    except Exception as e:
        print(f"[ssh-server] handler error on {cmd!r}: {e}", flush=True)
    finally:
        try:
            channel.close()
        except Exception:
            pass


def _scp_serve(channel, command, files):
    """Handle a single SCP server-side exec on `channel`.

    `command` is e.g. "scp -t /sftp/scp_upload.txt" (we receive a file
    the client uploads) or "scp -f /sftp/scp_download.txt" (we send a
    file the client downloads). `files` is the in-memory dict from
    _InMemorySFTPServer so SCP and SFTP share the same backing store.

    Returns the new contents of `files` (unchanged on -f, updated on -t).
    """
    import os.path as _op

    def _recvn(n):
        out = b""
        while len(out) < n:
            chunk = channel.recv(n - len(out))
            if not chunk:
                return out
            out += chunk
        return out

    def _recvline():
        out = b""
        while True:
            b = channel.recv(1)
            if not b:
                return out
            out += b
            if b == b"\n":
                return out

    # libssh2 sends e.g. `scp -pf '/path'` (combined flags + quoted path),
    # not just `scp -f /path`. Parse via shlex to honor the quoting, then
    # look at the flag bundle for `t` (receive) or `f` (send).
    import shlex
    try:
        parts = shlex.split(command)
    except ValueError:
        return False
    if len(parts) < 3 or parts[0] != "scp":
        return False
    flags, path = parts[1], parts[-1]
    if not flags.startswith("-"):
        return False
    preserve_times = "p" in flags
    if "t" in flags:
        mode = "-t"
    elif "f" in flags:
        mode = "-f"
    else:
        return False

    if mode == "-t":  # server receives
        channel.send(b"\x00")  # ready
        header = _recvline()
        # Format: "C<octal-mode> <size> <basename>\n"
        if not header.startswith(b"C"):
            return False
        try:
            _modepart, sizepart, _name = header.rstrip(b"\n").decode().split(" ", 2)
            size = int(sizepart)
        except (ValueError, IndexError):
            return False
        channel.send(b"\x00")
        data = _recvn(size)
        if len(data) < size:
            return False
        # Persist before waiting for the trailing \0: the client may
        # close the channel without sending one (libssh2's scp_send
        # doesn't, after sending the final \0 byte through write).
        files[path] = data
        nul = _recvn(1)  # SCP end-of-data \0 — best effort
        if nul == b"\x00":
            channel.send(b"\x00")  # final ack
        return True

    if mode == "-f":  # server sends
        ack = _recvn(1)
        if ack != b"\x00":
            return False
        data = files.get(path, b"")
        basename = _op.basename(path)
        # When the client uses `-p` (libssh2's scp_recv2 with non-NULL
        # sb), it expects a `T<mtime> 0 <atime> 0\n` time-info line
        # first. We mostly call scp_recv2 with NULL sb so this branch
        # is rarely taken, but support it for completeness.
        if preserve_times:
            channel.send(b"T1700000000 0 1700000000 0\n")
            ack = _recvn(1)
            if ack != b"\x00":
                return False
        header = f"C0644 {len(data)} {basename}\n".encode("ascii")
        channel.send(header)
        ack = _recvn(1)
        if ack != b"\x00":
            return False
        channel.send(data)
        channel.send(b"\x00")
        # Don't wait for a final \0 ack from the client. libssh2's
        # scp_recv2 doesn't send one after consuming data; waiting
        # would deadlock with the client's "read until EOF" loop.
        return True

    return False


class _InMemorySFTPServer(paramiko.SFTPServerInterface):
    """In-RAM SFTP backend so the rig has full control over what the
    client reads / writes. `files` is a {path: bytes} dict (None value
    marks a directory) the rig seeds before the client connects and
    inspects after. Path model is flat-with-/ — `/sftp/x.txt` lives
    under directory `/sftp`."""

    files = {}  # populated by serve_one() before transport.start_server
    dirs = set()  # known directory paths, e.g. {"/", "/sftp", "/scp"}

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

    def _is_dir(self, path):
        if path in _InMemorySFTPServer.dirs:
            return True
        prefix = path.rstrip("/") + "/"
        for f in _InMemorySFTPServer.files:
            if f.startswith(prefix):
                return True
        return False

    def _attrs_for(self, path, data=None):
        a = paramiko.SFTPAttributes()
        if self._is_dir(path):
            a.st_mode = 0o040755
            a.st_size = 0
        else:
            a.st_mode = 0o100644
            a.st_size = len(data if data is not None
                            else _InMemorySFTPServer.files.get(path, b""))
        a.st_uid = 0
        a.st_gid = 0
        a.st_atime = 0
        a.st_mtime = 0
        return a

    def stat(self, path):
        if path in _InMemorySFTPServer.files:
            return self._attrs_for(path)
        if self._is_dir(path):
            return self._attrs_for(path)
        return paramiko.SFTP_NO_SUCH_FILE

    def lstat(self, path):
        return self.stat(path)

    def list_folder(self, path):
        if not self._is_dir(path):
            return paramiko.SFTP_NO_SUCH_FILE
        prefix = path.rstrip("/") + "/"
        out = []
        seen = set()
        for f, data in _InMemorySFTPServer.files.items():
            if not f.startswith(prefix):
                continue
            tail = f[len(prefix):]
            if "/" in tail:
                # Subdirectory entry — emit the segment once.
                name = tail.split("/", 1)[0]
                if name in seen:
                    continue
                seen.add(name)
                a = paramiko.SFTPAttributes()
                a.st_mode = 0o040755
                a.st_size = 0
                a.filename = name
            else:
                if tail in seen:
                    continue
                seen.add(tail)
                a = self._attrs_for(f, data)
                a.filename = tail
            out.append(a)
        return out

    def mkdir(self, path, attr):
        _InMemorySFTPServer.dirs.add(path.rstrip("/") or "/")
        return paramiko.SFTP_OK

    def rmdir(self, path):
        p = path.rstrip("/") or "/"
        _InMemorySFTPServer.dirs.discard(p)
        return paramiko.SFTP_OK

    def remove(self, path):
        if path in _InMemorySFTPServer.files:
            del _InMemorySFTPServer.files[path]
            return paramiko.SFTP_OK
        return paramiko.SFTP_NO_SUCH_FILE

    def rename(self, oldpath, newpath):
        if oldpath in _InMemorySFTPServer.files:
            _InMemorySFTPServer.files[newpath] = \
                _InMemorySFTPServer.files.pop(oldpath)
            return paramiko.SFTP_OK
        if oldpath in _InMemorySFTPServer.dirs:
            _InMemorySFTPServer.dirs.discard(oldpath)
            _InMemorySFTPServer.dirs.add(newpath)
            return paramiko.SFTP_OK
        return paramiko.SFTP_NO_SUCH_FILE


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
    global CLIENT_PUBKEY
    host_key = paramiko.Ed25519Key(filename=str(host_key_path))
    client_pub_path = host_key_path.parent / "ssh_client_ed25519.pub"
    if client_pub_path.exists():
        # ssh-keygen .pub format is "ssh-ed25519 BASE64 comment"; the
        # middle field is the same SSH wire blob paramiko's
        # Ed25519Key takes via from_string_data.
        import base64
        parts = client_pub_path.read_text().split()
        if len(parts) >= 2 and parts[0] == "ssh-ed25519":
            blob = base64.b64decode(parts[1])
            CLIENT_PUBKEY = paramiko.Ed25519Key(data=blob)
            print(f"[ssh-server] loaded client pubkey: {parts[2] if len(parts) >= 3 else ''}",
                  flush=True)

    # Seed an SFTP file the rig client will fetch + verify, and pre-
    # create the path the client will upload to. The SCP rig shares the
    # same dict.
    _InMemorySFTPServer.files["/sftp/download.txt"] = b"SFTP_RIG_OK_DL\n"
    _InMemorySFTPServer.files["/sftp/upload.txt"] = b""
    _InMemorySFTPServer.files["/scp/download.txt"] = b"SCP_RIG_OK_DL\n"
    _InMemorySFTPServer.files["/scp/upload.txt"] = b""
    # Seed an entry the FS-ops slice of SSHTEST.PY can rename/unlink.
    _InMemorySFTPServer.files["/sftp/scratch.txt"] = b"scratch\n"
    _InMemorySFTPServer.dirs.update({"/", "/sftp", "/scp"})

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    sock.listen(2)
    print(f"[ssh-server] listening on 0.0.0.0:{port}", flush=True)

    # Two sequential connections expected: one for password auth +
    # exec/sftp/scp coverage, a second for publickey auth. The
    # second-connection wait shares the same deadline budget.
    deadline = time.time() + max_seconds
    conn_idx = 0
    while time.time() < deadline:
        conn_idx += 1
        remaining = max(deadline - time.time(), 0.5)
        sock.settimeout(remaining)
        try:
            client, addr = sock.accept()
        except socket.timeout:
            print(f"[ssh-server] no more connections after #{conn_idx - 1}",
                  flush=True)
            break
        print(f"[ssh-server] connection #{conn_idx} from {addr}", flush=True)

        transport = paramiko.Transport(client)
        transport.add_server_key(host_key)
        transport.set_subsystem_handler("sftp", paramiko.SFTPServer,
                                         _InMemorySFTPServer)
        server = Server(_InMemorySFTPServer.files)
        try:
            transport.start_server(server=server)
        except paramiko.SSHException as e:
            print(f"[ssh-server] handshake #{conn_idx} failed: {e}",
                  flush=True)
            continue
        print(f"[ssh-server] handshake #{conn_idx} ok; cipher={transport.local_cipher}",
              flush=True)

        # Exec channels (plain + SCP) are dispatched per-channel by
        # check_channel_exec_request → daemon thread. SFTP runs in
        # paramiko's subsystem-handler thread. Spin until this
        # transport closes or the global deadline fires; then loop
        # back for the next connection.
        while transport.is_active() and time.time() < deadline:
            time.sleep(0.5)
        try:
            transport.close()
        except Exception:
            pass
        print(f"[ssh-server] transport #{conn_idx} closed", flush=True)

    print("[ssh-server] all transports closed; "
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
