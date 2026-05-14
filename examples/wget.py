"""Minimal wget replacement for MicroPython under uc386-dos.

Streams an HTTP or HTTPS URL to disk. Built against MP's
`socket` (lwIP-backed), `ssl` (axtls-backed), and
`urllib.parse` (port-provided). Follows up to 5 redirects;
HTTP/1.0 + Connection: close keeps the decoder simple
(no chunked transfer to handle).

Usage from the REPL or `MP.EXE WGET.PY URL`:

    import wget
    wget.main(["-O", "OUT.TXT", "https://host/path"])

Or programmatically:

    from wget import fetch
    status, n, path = fetch("https://host/path", out="out.bin")

Flags:
    -O <path>          write to <path> instead of basename of URL
    --verify           require server-cert verification
    --ca-certs <pem>   CA bundle (PEM) for verify_mode=CERT_REQUIRED
                       (implies --verify)
    -h, --help

Memory: streams in 4 KB chunks; the response body is never
held in RAM in full. Headers are buffered up to the
header/body separator (small).

Limitations vs GNU wget:
    - No chunked transfer-encoding decode (HTTP/1.0 sidesteps it).
    - No keep-alive / connection reuse.
    - No resume (Range: requests).
    - No progress bar / rate display.
    - No proxy support.
    - Naive relative-redirect resolver (sufficient for absolute
      and abs-path redirects, which is the common case).
"""

import sys
import socket

try:
    import ssl as _ssl
except ImportError:  # pragma: no cover — port always has it
    _ssl = None

try:
    # CPython, and ports whose import system supports dotted
    # submodules at the C level.
    from urllib.parse import urlsplit
except ImportError:
    # uc386-dos registers `urllib_parse` as a top-level module
    # (its `urllib` package shim only exposes `.parse` as an
    # attribute, which dotted import doesn't always resolve to).
    from urllib_parse import urlsplit


def _split_url(url):
    """Return (scheme, host, port, path) for an http(s):// URL."""
    p = urlsplit(url)
    scheme = (p.scheme or "http").lower()
    netloc = p.netloc
    # Strip optional userinfo@.
    if "@" in netloc:
        netloc = netloc.split("@", 1)[1]
    if ":" in netloc:
        host, port_s = netloc.rsplit(":", 1)
        port = int(port_s)
    else:
        host = netloc
        port = 443 if scheme == "https" else 80
    path = p.path or "/"
    if p.query:
        path += "?" + p.query
    return scheme, host, port, path


def _connect(scheme, host, port, *, verify, ca_certs):
    """Open the TCP socket (and TLS wrap for https://). Returns
    the read/write end of the connection, ready for HTTP."""
    info = socket.getaddrinfo(host, port)
    addr = info[0][-1]
    sock = socket.socket()
    sock.connect(addr)

    if scheme != "https":
        return sock

    if _ssl is None:
        sock.close()
        raise OSError("ssl module not available — build with MICROPY_PY_SSL=1")

    ctx = _ssl.SSLContext(_ssl.PROTOCOL_TLS_CLIENT)
    if verify:
        ctx.verify_mode = _ssl.CERT_REQUIRED
        if ca_certs is not None:
            with open(ca_certs, "rb") as f:
                cadata = f.read()
            # MP/axtls's `load_verify_locations(cadata=...)` accepts
            # raw PEM bytes. Host CPython treats `cadata=bytes` as
            # DER-encoded and `cadata=str` as PEM; route by content
            # so the same wget call works on either side.
            if b"-----BEGIN" in cadata:
                try:
                    ctx.load_verify_locations(cadata=cadata.decode("ascii"))
                except (TypeError, ValueError):
                    # MP's axtls signature only takes positional /
                    # bytes — fall back to the bytes form.
                    ctx.load_verify_locations(cadata=cadata)
            else:
                ctx.load_verify_locations(cadata=cadata)
    else:
        ctx.verify_mode = _ssl.CERT_NONE
    # axtls accepts but ignores `server_hostname`; pass it so the
    # same source compiles against host CPython + ports that DO
    # honour SNI (mbedtls, openssl-shim).
    try:
        return ctx.wrap_socket(sock, server_hostname=host)
    except TypeError:
        return ctx.wrap_socket(sock)


def _build_request(host, path, headers):
    lines = [
        b"GET " + path.encode("ascii") + b" HTTP/1.0",
        b"Host: " + host.encode("ascii"),
        b"User-Agent: uc386-wget/1.0",
        b"Accept: */*",
        b"Connection: close",
    ]
    if headers:
        for k, v in headers.items():
            lines.append(k.encode("ascii") + b": " + v.encode("ascii"))
    return b"\r\n".join(lines) + b"\r\n\r\n"


def _read_headers(sock):
    """Read until we see the CRLFCRLF separator. Returns
    (status_int, headers_dict, leftover_body_bytes)."""
    buf = bytearray()
    while True:
        sep = buf.find(b"\r\n\r\n")
        if sep >= 0:
            break
        chunk = sock.read(512)
        if not chunk:
            break
        buf.extend(chunk)
    sep = buf.find(b"\r\n\r\n")
    if sep < 0:
        raise OSError("incomplete HTTP response (no header terminator)")
    hdr_bytes = bytes(buf[:sep])
    leftover = bytes(buf[sep + 4:])

    lines = hdr_bytes.split(b"\r\n")
    status_parts = lines[0].split(b" ", 2)
    if len(status_parts) < 2:
        raise OSError("malformed HTTP status line: " + repr(lines[0]))
    try:
        status = int(status_parts[1])
    except ValueError:
        raise OSError("non-numeric HTTP status: " + repr(status_parts[1]))

    hdrs = {}
    for ln in lines[1:]:
        kv = ln.split(b":", 1)
        if len(kv) != 2:
            continue
        key = kv[0].strip().lower().decode("ascii", "replace")
        val = kv[1].strip().decode("ascii", "replace")
        hdrs[key] = val
    return status, hdrs, leftover


def _resolve_redirect(prev_scheme, prev_host, prev_path, location):
    if "://" in location:
        return location
    base = prev_scheme + "://" + prev_host
    if location.startswith("/"):
        return base + location
    # Same-directory relative: drop the last path segment.
    return base + prev_path.rsplit("/", 1)[0] + "/" + location


def fetch(url, out=None, headers=None, verify=False, ca_certs=None,
          buf_size=4096, max_redirects=5):
    """Download `url` to `out` (or the URL's basename if `out` is
    None). Returns `(status, bytes_written, path)`. Raises OSError
    on transport or HTTP-status failure."""
    redirects = 0
    while True:
        scheme, host, port, path = _split_url(url)
        sock = _connect(scheme, host, port,
                        verify=verify, ca_certs=ca_certs)
        try:
            sock.write(_build_request(host, path, headers))
            status, resp_hdrs, leftover = _read_headers(sock)

            if 300 <= status < 400 and "location" in resp_hdrs:
                if redirects >= max_redirects:
                    raise OSError("too many redirects (%d)" % max_redirects)
                url = _resolve_redirect(scheme, host, path,
                                        resp_hdrs["location"])
                redirects += 1
                # Drain & close the current socket before reconnecting.
                continue

            if status != 200:
                raise OSError("HTTP %d" % status)

            sink = out
            if sink is None:
                sink = path.rsplit("/", 1)[-1] or "index.html"
            total = 0
            with open(sink, "wb") as f:
                if leftover:
                    f.write(leftover)
                    total += len(leftover)
                while True:
                    chunk = sock.read(buf_size)
                    if not chunk:
                        break
                    f.write(chunk)
                    total += len(chunk)
            return status, total, sink
        finally:
            try:
                sock.close()
            except Exception:
                pass


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    out = None
    verify = False
    ca_certs = None
    url = None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "-O" and i + 1 < len(argv):
            out = argv[i + 1]
            i += 2
        elif a == "--verify":
            verify = True
            i += 1
        elif a == "--ca-certs" and i + 1 < len(argv):
            ca_certs = argv[i + 1]
            verify = True
            i += 2
        elif a in ("-h", "--help"):
            print("usage: wget.py [-O OUT] [--verify] [--ca-certs CA.PEM] URL")
            return 0
        elif a.startswith("-"):
            sys.stderr.write("unknown flag: " + a + "\n")
            return 2
        else:
            url = a
            i += 1
    if url is None:
        sys.stderr.write(
            "usage: wget.py [-O OUT] [--verify] [--ca-certs CA.PEM] URL\n"
        )
        return 2
    try:
        status, total, path = fetch(
            url, out=out, verify=verify, ca_certs=ca_certs,
        )
    except OSError as e:
        sys.stderr.write("wget: " + str(e) + "\n")
        return 1
    print("wrote " + str(total) + " bytes to " + path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
