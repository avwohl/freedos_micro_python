import uc386_net
print('NETBASE: start')
print('NETBASE: pre_eth_init')
rc = uc386_net.eth_init(False)
print('NETBASE: eth_init rc=', rc)
ip, nm, gw, up, drv = uc386_net.eth_status()
print('NETBASE: drv=', drv)
print('NETBASE: up=', up)
if drv == 'pktdrv' and up:
    print('NETBASE: PASS')
else:
    print('NETBASE: FAIL')
