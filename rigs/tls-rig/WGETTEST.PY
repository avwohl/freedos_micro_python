import sys
import time
import lwip
import uc386_net
import socket
import ssl
try:
    from urllib_parse import urlsplit
except ImportError:
    from urllib.parse import urlsplit
print('WGETTEST: start')

# Skip wget.py import: under PMODE/W the lex+parse+compile of an
# external 9 KB Python source from the floppy hangs (MP's frozen-
# import / compile path serializes with INT 21h). Inline the small
# subset we exercise here.
#
# Also skip ALL DOS file IO: empirically, INT 21h AH=3D (open) hangs
# deterministically once uc386_net.eth_init() unmasks IRQ 9 for the
# Crynwr packet driver, regardless of socket state. The rig script
# substitutes the CA root in as a bytes literal at floppy-build
# time so MP never has to touch the filesystem after MP startup.
CA_PEM = __CA_PEM_BYTES__

def _split_url(url):
    p = urlsplit(url)
    scheme = (p.scheme or "http").lower()
    netloc = p.netloc
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

def fetch_verified_to_ram(url, ca_pem):
    scheme, host, port, path = _split_url(url)
    addr = socket.getaddrinfo(host, port)[0][-1]
    sk = socket.socket()
    sk.connect(addr)
    print('WGETTEST: tcp_connected')
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.load_verify_locations(cadata=ca_pem)
    print('WGETTEST: ca_loaded_into_ctx')
    sk = ctx.wrap_socket(sk)
    print('WGETTEST: tls_handshake_ok')
    req = (
        b"GET " + path.encode("ascii") + b" HTTP/1.0\r\n"
        + b"Host: " + host.encode("ascii") + b"\r\n"
        + b"Connection: close\r\n\r\n"
    )
    sk.write(req)
    buf = bytearray()
    while True:
        idx = buf.find(b"\r\n\r\n")
        if idx >= 0:
            break
        ch = sk.read(256)
        if not ch:
            break
        buf.extend(ch)
    idx = buf.find(b"\r\n\r\n")
    hdr = bytes(buf[:idx])
    body = bytearray(buf[idx + 4:])
    parts = hdr.split(b"\r\n", 1)[0].split(b" ", 2)
    status = int(parts[1])
    print('WGETTEST: http_status', status)
    while True:
        ch = sk.read(512)
        if not ch:
            break
        body.extend(ch)
    sk.close()
    return status, bytes(body)

# Network bring-up.
_ = lwip.reset()
rc = uc386_net.eth_init(False)
uc386_net.eth_set_static('10.0.2.15', '255.255.255.0', '10.0.2.2')
for _ in range(5):
    _ = lwip.callback()
    time.sleep_ms(50)
info = uc386_net.eth_status()
ip = info[0]
up = info[3]
print('WGETTEST: ifup', up, 'ip', ip)
if not up or not ip or ip == '0.0.0.0':
    print('WGETTEST: static_ip_failed')
    sys.exit(1)

print('WGETTEST: pre_fetch ca_len=', len(CA_PEM))
try:
    status, body = fetch_verified_to_ram(
        'https://10.0.2.2:8443/wget.bin', CA_PEM,
    )
except OSError as e:
    print('WGETTEST: fetch_failed', e)
    sys.exit(2)
print('WGETTEST: post_fetch status=', status, 'body_len=', len(body))
if status == 200 and b'WGET_RIG_OK' in body and len(body) > 11:
    print('WGETTEST: PASS')
else:
    print('WGETTEST: FAIL_unexpected_response body_len=', len(body))
