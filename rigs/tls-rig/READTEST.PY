import sys
import time
import lwip
import uc386_net
print('READTEST: start')
print('READTEST: pre_lwip_reset')
_ = lwip.reset()
print('READTEST: post_lwip_reset')
print('READTEST: pre_eth_init')
rc = uc386_net.eth_init(False)
print('READTEST: post_eth_init')
sys.stdout.write('READTEST: rc_eq_zero=' + str(rc == 0) + '\n')
uc386_net.eth_set_static('10.0.2.15', '255.255.255.0', '10.0.2.2')
print('READTEST: post_set_static')
for tick in range(5):
    _ = lwip.callback()
    time.sleep_ms(50)
print('READTEST: pre_open')
f = open('TESTCA.PEM', 'rb')
print('READTEST: post_open')
print('READTEST: pre_read')
data = f.read(64)
print('READTEST: post_read len=', len(data))
print('READTEST: first16=', data[:16])
f.close()
print('READTEST: PASS')
