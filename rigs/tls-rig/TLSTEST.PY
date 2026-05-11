import sys
import time
import lwip
import uc386_net
import socket
import ssl
print('TLSTEST: start')
print('TLSTEST: pre_lwip_reset')
_ = lwip.reset()
print('TLSTEST: post_lwip_reset')
print('TLSTEST: pre_eth_init')
# Bring up the netif WITHOUT DHCP, then set a static IP that matches
# QEMU's SLIRP user-mode network (10.0.2.15/24, gateway 10.0.2.2).
# This sidesteps the lwIP DHCP retry timer that fires inside
# sys_check_timeouts at ~2 s — that path was deterministically
# hanging under PMODE/W (see CI run 25549376977 markers).
rc = uc386_net.eth_init(False)
print('TLSTEST: post_eth_init')
sys.stdout.write('TLSTEST: rc_eq_zero=' + str(rc == 0) + '\n')
uc386_net.eth_set_static('10.0.2.15', '255.255.255.0', '10.0.2.2')
print('TLSTEST: post_set_static')
diag0 = uc386_net.pktdrv_diag()
sys.stdout.write('TLSTEST: diag_init thunk=' + str(diag0[0]) + ' p0=' + str(diag0[1]) + ' p1=' + str(diag0[2]) + ' handle=' + str(diag0[3]) + ' tramp=' + str(diag0[4]) + ':' + str(diag0[5]) + '\n')
# A few callback ticks so the link comes up cleanly, but no long
# DHCP wait — IP is already configured.
for tick in range(5):
    _ = lwip.callback()
    time.sleep_ms(50)
info = uc386_net.eth_status()
ip = info[0]
up = info[3]
print('TLSTEST: ifup', up, 'ip', ip)
if not up or not ip or ip == '0.0.0.0':
    print('TLSTEST: static_ip_failed')
    sys.exit(1)
print('TLSTEST: pre_getaddrinfo')
addr = socket.getaddrinfo('10.0.2.2', 8443)[0][-1]
print('TLSTEST: post_getaddrinfo')
s = socket.socket()
print('TLSTEST: post_socket')
# Use blocking connect — MICROPY_PY_LWIP_POLL_HOOK in modlwip's wait
# loop pumps our RX, so the SYN-ACK gets delivered to lwIP and
# _lwip_tcp_connected transitions the socket to STATE_CONNECTED.
try:
    s.connect(addr)
except OSError as e:
    print('TLSTEST: connect_failed', e)
    sys.exit(2)
print('TLSTEST: tcp_connected')
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
print('TLSTEST: ctx_created')
ctx.verify_mode = ssl.CERT_NONE     # skip cert verification for now
print('TLSTEST: vmode_set')
print('TLSTEST: ctx_ready')
try:
    tls = ctx.wrap_socket(s)
except OSError as e:
    print('TLSTEST: handshake_failed', e)
    sys.exit(3)
print('TLSTEST: handshake_ok')
_ = tls.write(b'hi-from-uc386')
data = tls.read(64)
print('TLSTEST: received', data)
if b'TLS_RIG_OK' in data:
    print('TLSTEST: PASS')
else:
    print('TLSTEST: FAIL_unexpected_response')
