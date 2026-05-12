import uc386_net
import lwip
import time
print('NETBASE: start')
rc = uc386_net.eth_init(True)
print('NETBASE: eth_init rc=', rc)
for i in range(80):
    lwip.callback()
    ip, nm, gw, up, drv = uc386_net.eth_status()
    if ip != '0.0.0.0':
        print('NETBASE: got-ip after', i, 'polls')
        break
    time.sleep_ms(50)
ip, nm, gw, up, drv = uc386_net.eth_status()
inv, p0, p1, h, sg, of = uc386_net.pktdrv_diag()
print('NETBASE: thunk_inv=', inv, 'phase0=', p0, 'phase1=', p1)
print('NETBASE: ip=', ip)
print('NETBASE: gw=', gw)
if ip != '0.0.0.0':
    print('NETBASE: PASS')
else:
    print('NETBASE: FAIL')
