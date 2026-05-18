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
# Cap blocking-recv with a 100 ms timeout so libssh2's `channel_write`
# drain-incoming loop (`while (transport_read > 0)`) can exit when
# nothing new is queued. Without this, the drain blocks forever
# waiting for a packet that won't come, and SFTP_INIT never goes out.
s.settimeout(0.1)
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
exec_pass = b'SSH_RIG_OK' in out

# SFTP round-trip on the same SSH session: download a seeded file
# then upload a marker the server can verify.
sftp_pass = False
sftp_fsops_pass = False
try:
    sftp = sess.sftp()
    print('SSHTEST: sftp_opened')
    f = sftp.open('/sftp/download.txt', 'r')
    dl = f.read()
    f.close()
    print('SSHTEST: sftp_dl_len=' + str(len(dl)))
    if b'SFTP_RIG_OK_DL' in dl:
        f = sftp.open('/sftp/upload.txt', 'w')
        n = f.write(b'SFTP_RIG_OK_UL\n')
        f.close()
        print('SSHTEST: sftp_ul_len=' + str(n))
        sftp_pass = (n == 15)

    # FS-ops smoke: realpath → opendir → stat → mkdir → rename → unlink → rmdir.
    # Each method touches a different wrapper in modssh_uc386dos.c.
    try:
        rp = sftp.realpath('/sftp')
        print('SSHTEST: sftp_realpath=' + rp)

        d = sftp.opendir('/sftp')
        names = []
        while True:
            e = d.read()
            if e is None:
                break
            names.append(e[0])
        d.close()
        print('SSHTEST: sftp_ls=' + ','.join(sorted(names)))

        st = sftp.stat('/sftp/download.txt')
        print('SSHTEST: sftp_stat_size=' + str(st[1]))

        sftp.mkdir('/sftp/newdir', 0o755)
        sftp.rename('/sftp/scratch.txt', '/sftp/renamed.txt')
        sftp.unlink('/sftp/renamed.txt')
        sftp.rmdir('/sftp/newdir')
        print('SSHTEST: sftp_fsops_ok')
        sftp_fsops_pass = (rp == '/sftp'
                            and 'download.txt' in names
                            and st[1] == len(dl))
    except OSError as e:
        print('SSHTEST: sftp_fsops_failed', e)

    sftp.close()
except OSError as e:
    print('SSHTEST: sftp_failed', e)

# SCP round-trip on the same SSH session.
scp_pass = False
try:
    dl = sess.scp_recv('/scp/download.txt')
    print('SSHTEST: scp_dl_len=' + str(len(dl)))
    scp_dl_pass = b'SCP_RIG_OK_DL' in dl
    payload = b'SCP_RIG_OK_UL\n'
    n = sess.scp_send('/scp/upload.txt', 0o644, payload)
    print('SSHTEST: scp_ul_len=' + str(n))
    scp_pass = scp_dl_pass and (n == len(payload))
except OSError as e:
    print('SSHTEST: scp_failed', e)

if exec_pass and sftp_pass and sftp_fsops_pass and scp_pass:
    # Leading \n so the rig's `grep -qE '^SSHTEST: PASS\r?$'` matches
    # even when the line gets prefixed by diagnostic markers from
    # the libssh2/lwIP/axtls code (e.g. [hu:...]).
    sys.stdout.write('\nSSHTEST: PASS\n')
else:
    sys.stdout.write('\nSSHTEST: FAIL_unexpected_response exec=' + str(exec_pass)
                     + ' sftp=' + str(sftp_pass)
                     + ' sftp_fsops=' + str(sftp_fsops_pass)
                     + ' scp=' + str(scp_pass) + '\n')
sess.close()
