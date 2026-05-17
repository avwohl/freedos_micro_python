import sys
import time
import lwip
import uc386_net
import socket
import _ssh
print('SSHTEST: start')
print('SSHTEST: version=' + str(_ssh.version()))
print('SSHTEST: engine=' + str(_ssh.crypto_engine_name()))
_ = lwip.reset()
print('SSHTEST: post_lwip_reset')
rc = uc386_net.eth_init(False)
sys.stdout.write('SSHTEST: rc_eq_zero=' + str(rc == 0) + '\n')
uc386_net.eth_set_static('10.0.2.15', '255.255.255.0', '10.0.2.2')
for tick in range(5):
    _ = lwip.callback()
    time.sleep_ms(50)
info = uc386_net.eth_status()
print('SSHTEST: ifup', info[3], 'ip', info[0])
if not info[3] or info[0] == '0.0.0.0':
    print('SSHTEST: static_ip_failed')
    sys.exit(1)
addr = socket.getaddrinfo('10.0.2.2', 2222)[0][-1]
s = socket.socket()
try:
    s.connect(addr)
except OSError as e:
    print('SSHTEST: connect_failed', e)
    sys.exit(2)
print('SSHTEST: tcp_connected')
try:
    sess = _ssh.Session(s)
except OSError as e:
    print('SSHTEST: session_failed', e)
    sys.exit(3)
print('SSHTEST: handshake_ok')
try:
    sess.userauth_password('testuser', 'testpass')
except OSError as e:
    print('SSHTEST: auth_failed', e)
    sys.exit(4)
print('SSHTEST: auth_ok')
try:
    out = sess.exec('echo SSH_RIG_OK')
except OSError as e:
    print('SSHTEST: exec_failed', e)
    sys.exit(5)
print('SSHTEST: exec_ok len=' + str(len(out)))
if b'SSH_RIG_OK' in out:
    print('SSHTEST: PASS')
else:
    print('SSHTEST: FAIL_unexpected_response')
sess.close()
