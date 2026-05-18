"""Interactive sftp client for MicroPython under uc386-dos.

Like OpenSSH's `sftp(1)`: connect once, then a command prompt accepts
ls / cd / get / put / mkdir / rm / mv / chmod / stat / exit / etc.

Usage from the REPL or `MP.EXE SFTP.PY`:

    # Interactive
    sftp.main(["user@host"])
        sftp> ls
        sftp> cd /var/log
        sftp> get messages
        sftp> exit

    # One-shot: any positional arg with a host:/path colon dispatches
    # automatically based on direction.
    sftp.main(["host:/etc/motd", "MOTD.TXT"])    # get
    sftp.main(["DATA.BIN", "host:/uploads"])     # put

    # Batch mode reads newline-separated commands from a file then exits.
    sftp.main(["-b", "BATCH.TXT", "user@host"])

Flags:
    -P <port>        SSH port (default 22)
    -u <user>        login user (default "testuser")
    -p <password>    password (default "testpass")
    -b <file>        batch file (one command per line)
    -h, --help

Commands implemented at the sftp> prompt:
    ls [path]                 list remote directory
    cd [path]                 change remote directory
    lcd [path]                change local directory
    pwd                       print remote cwd
    lpwd                      print local cwd
    get remote [local]        download a file
    put local [remote]        upload a file
    mkdir path [mode]         create remote directory
    rmdir path                remove empty remote directory
    rm path / del path        remove remote file
    mv old new                rename remote file/dir
    chmod MODE path           change remote permissions
    stat path                 print remote attrs (mode/size/atime/mtime)
    realpath path             canonicalize remote path
    exit / quit / bye         disconnect
    help / ?                  show this list
    # ...                     comment (ignored)

Built on `_ssh.Session.sftp()` + the SFTP/SFTPFile/SFTPDir wrappers in
`port/modssh_uc386dos.c`. Password auth only for now.
"""

import sys
import socket
import _ssh


# Bit positions used by libssh2_sftp_stat's flag field.
_S_IFMT  = 0o170000
_S_IFDIR = 0o040000
_S_IFLNK = 0o120000
_S_IFREG = 0o100000


def _split_remote(spec):
    """`[user@]host[:path]` → (host, path|None). If path absent → None."""
    if ":" in spec:
        host, _, path = spec.partition(":")
        path = path or None
    else:
        host, path = spec, None
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


def _resolve(cwd, path):
    """Compose a remote-cwd-relative path. Absolute path passes through;
    "." → cwd; otherwise joined as cwd/path."""
    if path is None or path == "":
        return cwd
    if path.startswith("/"):
        return path
    if path == ".":
        return cwd
    if cwd.endswith("/"):
        return cwd + path
    return cwd + "/" + path


def _fmt_mode(mode):
    """ls-style `drwxr-xr-x` (10 chars) from a unix mode bitfield."""
    out = []
    f = mode & _S_IFMT
    if f == _S_IFDIR:
        out.append("d")
    elif f == _S_IFLNK:
        out.append("l")
    else:
        out.append("-")
    for shift in (6, 3, 0):
        b = (mode >> shift) & 7
        out.append("r" if b & 4 else "-")
        out.append("w" if b & 2 else "-")
        out.append("x" if b & 1 else "-")
    return "".join(out)


# --- Commands ---------------------------------------------------------

class Shell:
    def __init__(self, sftp):
        self.sftp = sftp
        try:
            self.cwd = sftp.realpath(".")
        except OSError:
            self.cwd = "/"
        self.lcwd = ""  # local cwd tracker (DOS has no os.chdir-equivalent)

    def _r(self, path):
        return _resolve(self.cwd, path)

    def cmd_ls(self, args):
        target = self._r(args[0]) if args else self.cwd
        d = self.sftp.opendir(target)
        try:
            entries = []
            while True:
                e = d.read()
                if e is None:
                    break
                entries.append(e)
        finally:
            d.close()
        for name, attrs in entries:
            mode, size, atime, mtime, uid, gid = attrs
            print(_fmt_mode(mode) + " " + str(size).rjust(10) + " " + name)

    def cmd_cd(self, args):
        new = self._r(args[0]) if args else "/"
        try:
            self.cwd = self.sftp.realpath(new)
        except OSError as e:
            print("cd: " + str(e))

    def cmd_lcd(self, args):
        self.lcwd = args[0] if args else ""
        print("local cwd: " + (self.lcwd or "<cwd>"))

    def cmd_pwd(self, args):
        print("remote: " + self.cwd)

    def cmd_lpwd(self, args):
        print("local: " + (self.lcwd or "<cwd>"))

    def cmd_get(self, args):
        if not args:
            print("usage: get remote [local]")
            return
        remote = self._r(args[0])
        local = args[1] if len(args) > 1 else args[0].rsplit("/", 1)[-1]
        if self.lcwd and not local.startswith("/"):
            local = self.lcwd + "/" + local
        f = self.sftp.open(remote, "r")
        try:
            data = f.read()
        finally:
            f.close()
        with open(local, "wb") as out:
            out.write(data)
        print("got " + str(len(data)) + " bytes to " + local)

    def cmd_put(self, args):
        if not args:
            print("usage: put local [remote]")
            return
        local = args[0]
        if self.lcwd and not local.startswith("/"):
            local = self.lcwd + "/" + local
        remote_arg = args[1] if len(args) > 1 else local.rsplit("/", 1)[-1]
        remote = self._r(remote_arg)
        with open(local, "rb") as f:
            data = f.read()
        f = self.sftp.open(remote, "w")
        try:
            n = f.write(data)
        finally:
            f.close()
        print("sent " + str(n) + " bytes to " + remote)

    def cmd_mkdir(self, args):
        if not args:
            print("usage: mkdir path [mode]")
            return
        mode = int(args[1], 8) if len(args) > 1 else 0o755
        self.sftp.mkdir(self._r(args[0]), mode)

    def cmd_rmdir(self, args):
        if not args:
            print("usage: rmdir path")
            return
        self.sftp.rmdir(self._r(args[0]))

    def cmd_rm(self, args):
        if not args:
            print("usage: rm path")
            return
        self.sftp.unlink(self._r(args[0]))

    def cmd_mv(self, args):
        if len(args) != 2:
            print("usage: mv old new")
            return
        self.sftp.rename(self._r(args[0]), self._r(args[1]))

    def cmd_chmod(self, args):
        # libssh2 doesn't expose setstat through our wrapper yet — for
        # now report unsupported rather than silently ignore.
        print("chmod: not implemented (libssh2 setstat not yet wrapped)")

    def cmd_stat(self, args):
        if not args:
            print("usage: stat path")
            return
        attrs = self.sftp.stat(self._r(args[0]))
        mode, size, atime, mtime, uid, gid = attrs
        print("mode  " + oct(mode) + "  (" + _fmt_mode(mode) + ")")
        print("size  " + str(size))
        print("atime " + str(atime))
        print("mtime " + str(mtime))
        print("uid   " + str(uid))
        print("gid   " + str(gid))

    def cmd_realpath(self, args):
        if not args:
            print("usage: realpath path")
            return
        print(self.sftp.realpath(self._r(args[0])))

    def cmd_help(self, args):
        for line in __doc__.splitlines():
            ls = line.lstrip()
            if ls.startswith(("ls ", "cd ", "lcd ", "pwd", "lpwd",
                              "get ", "put ", "mkdir ", "rmdir ",
                              "rm ", "mv ", "chmod ", "stat ",
                              "realpath ", "exit ", "help ", "# ...")):
                print(line.rstrip())


_DISPATCH = {
    "ls": "cmd_ls", "cd": "cmd_cd", "lcd": "cmd_lcd",
    "pwd": "cmd_pwd", "lpwd": "cmd_lpwd",
    "get": "cmd_get", "put": "cmd_put",
    "mkdir": "cmd_mkdir", "rmdir": "cmd_rmdir",
    "rm": "cmd_rm", "del": "cmd_rm", "delete": "cmd_rm",
    "mv": "cmd_mv", "rename": "cmd_mv",
    "chmod": "cmd_chmod",
    "stat": "cmd_stat", "realpath": "cmd_realpath",
    "help": "cmd_help", "?": "cmd_help",
}


def _execute_one(shell, line):
    line = line.strip()
    if not line or line.startswith("#"):
        return True
    if line in ("exit", "quit", "bye"):
        return False
    parts = line.split()
    cmd, args = parts[0], parts[1:]
    handler_name = _DISPATCH.get(cmd)
    if handler_name is None:
        print("unknown command: " + cmd + " (try 'help')")
        return True
    try:
        getattr(shell, handler_name)(args)
    except OSError as e:
        print(cmd + ": " + str(e))
    return True


def interactive(shell, source=None):
    """Run the command loop. `source` is an iterable of lines (batch mode);
    if None, read stdin via input() with prompts."""
    if source is None:
        while True:
            try:
                line = input("sftp> ")
            except EOFError:
                print("")
                return
            if not _execute_one(shell, line):
                return
    else:
        for line in source:
            line = line.rstrip("\n").rstrip("\r")
            print("sftp> " + line)
            if not _execute_one(shell, line):
                return


# --- Entry point ------------------------------------------------------

def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    port = 22
    user = "testuser"
    password = "testpass"
    batch = None
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
        elif a == "-b" and i + 1 < len(argv):
            batch = argv[i + 1]; i += 2
        elif a in ("-h", "--help"):
            print("usage: sftp.py [-P PORT] [-u USER] [-p PASS] [-b BATCH]")
            print("               [user@]host                  # interactive")
            print("               host:/path local              # one-shot get")
            print("               local host:/path              # one-shot put")
            print("               get host:/path local          # explicit get")
            print("               put local host:/path          # explicit put")
            return 0
        elif a.startswith("-"):
            sys.stderr.write("unknown flag: " + a + "\n"); return 2
        else:
            positional.append(a); i += 1

    # Explicit "get"/"put" subcommands for back-compat.
    if positional and positional[0] in ("get", "put"):
        if len(positional) != 3:
            sys.stderr.write("usage: sftp.py get|put SRC DST\n"); return 2
        cmd, src, dst = positional
        host, _ = _split_remote(src if cmd == "get" else dst)
        sess = _connect(host, port, user, password)
        try:
            sftp = sess.sftp()
            shell = Shell(sftp)
            try:
                if cmd == "get":
                    _, remote = _split_remote(src)
                    shell.cmd_get([remote, dst])
                else:
                    _, remote = _split_remote(dst)
                    shell.cmd_put([src, remote])
            finally:
                sftp.close()
        finally:
            sess.close()
        return 0

    # Auto-detect one-shot get/put when exactly one positional arg has
    # a `host:/path` colon (sftp(1) compat).
    if len(positional) == 2:
        src_remote = ":" in positional[0]
        dst_remote = ":" in positional[1]
        if src_remote and not dst_remote:
            return main(["get"] + positional + ["-P", str(port),
                          "-u", user, "-p", password])
        if dst_remote and not src_remote:
            return main(["put"] + positional + ["-P", str(port),
                          "-u", user, "-p", password])

    # Interactive: positional[0] must be the host spec.
    if not positional:
        sys.stderr.write("usage: sftp.py [opts] [user@]host  (try -h)\n")
        return 2
    host, _ = _split_remote(positional[0])
    try:
        sess = _connect(host, port, user, password)
    except OSError as e:
        sys.stderr.write("sftp: connect: " + str(e) + "\n")
        return 1
    try:
        sftp = sess.sftp()
        shell = Shell(sftp)
        try:
            if batch is not None:
                with open(batch, "r") as f:
                    interactive(shell, source=f)
            else:
                print("Connected to " + host + ".")
                interactive(shell)
        finally:
            sftp.close()
    finally:
        sess.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
