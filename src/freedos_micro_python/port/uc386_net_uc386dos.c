// uc386-dos `uc386_net` module — control surface for the lwIP eth
// netif sitting on top of the INT 0x83 packet-driver shim.
//
// Surface (Python):
//   uc386_net.eth_init(use_dhcp=True) -> rc:int
//     Bring up the virtual eth netif. If use_dhcp, kicks off DHCP
//     discovery; the address won't be ready until enough
//     `lwip.callback()` ticks have run for the DHCP server to
//     respond. Returns 0 on success, negative on error.
//   uc386_net.eth_status() -> (ip, netmask, gateway, up:bool)
//     IP/netmask/gateway as dotted-quad strings (zero-string when
//     unset). `up` reflects netif_is_up() AND init has run.

#include <stdio.h>
#include <string.h>

#include "py/runtime.h"
#include "py/obj.h"

extern int uc386dos_eth_start(int dhcp_start_now);
extern void uc386dos_eth_set_addr(unsigned int ip, unsigned int mask, unsigned int gw);
extern unsigned int uc386dos_eth_ip(void);
extern unsigned int uc386dos_eth_netmask(void);
extern unsigned int uc386dos_eth_gateway(void);
extern int uc386dos_eth_is_up(void);
extern int uc386dos_eth_driver(void);
extern volatile unsigned int pktdrv_thunk_invocations;
extern volatile unsigned int pktdrv_thunk_phase0_count;
extern volatile unsigned int pktdrv_thunk_phase1_count;
extern unsigned int pktdrv_last_handle;
extern unsigned int pktdrv_dpmi_seg;
extern unsigned int pktdrv_dpmi_off;

static unsigned int parse_ip4(const char *s) {
    unsigned int parts[4] = {0};
    int idx = 0;
    while (*s && idx < 4) {
        unsigned int v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            s++;
        }
        parts[idx++] = v & 0xff;
        if (*s == '.') s++;
    }
    return parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
}

static mp_obj_t mod_uc386_net_ip4_to_str(unsigned int addr) {
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                     (unsigned)(addr & 0xff),
                     (unsigned)((addr >> 8) & 0xff),
                     (unsigned)((addr >> 16) & 0xff),
                     (unsigned)((addr >> 24) & 0xff));
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        n = 0;
    }
    return mp_obj_new_str(buf, (size_t)n);
}

static mp_obj_t mod_uc386_net_eth_init(size_t n_args, const mp_obj_t *args) {
    int use_dhcp = 1;
    if (n_args > 0) {
        use_dhcp = mp_obj_is_true(args[0]) ? 1 : 0;
    }
    int rc = uc386dos_eth_start(use_dhcp);
    return MP_OBJ_NEW_SMALL_INT(rc);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(
    mod_uc386_net_eth_init_obj, 0, 1, mod_uc386_net_eth_init);

static mp_obj_t mod_uc386_net_eth_status(void) {
    mp_obj_t items[5];
    items[0] = mod_uc386_net_ip4_to_str(uc386dos_eth_ip());
    items[1] = mod_uc386_net_ip4_to_str(uc386dos_eth_netmask());
    items[2] = mod_uc386_net_ip4_to_str(uc386dos_eth_gateway());
    items[3] = uc386dos_eth_is_up() ? mp_const_true : mp_const_false;
    // 0=none, 1="int83-sim", 2="pktdrv"
    int drv = uc386dos_eth_driver();
    const char *drv_name =
        (drv == 2) ? "pktdrv" :
        (drv == 1) ? "int83-sim" : "none";
    items[4] = mp_obj_new_str(drv_name, strlen(drv_name));
    return mp_obj_new_tuple(5, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(
    mod_uc386_net_eth_status_obj, mod_uc386_net_eth_status);

static mp_obj_t mod_uc386_net_eth_set_static(mp_obj_t ip_obj, mp_obj_t mask_obj, mp_obj_t gw_obj) {
    const char *ip_s   = mp_obj_str_get_str(ip_obj);
    const char *mask_s = mp_obj_str_get_str(mask_obj);
    const char *gw_s   = mp_obj_str_get_str(gw_obj);
    uc386dos_eth_set_addr(parse_ip4(ip_s), parse_ip4(mask_s), parse_ip4(gw_s));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(
    mod_uc386_net_eth_set_static_obj, mod_uc386_net_eth_set_static);

extern unsigned char pktdrv_int_invoke(unsigned int, unsigned int *);
static mp_obj_t mod_uc386_net_dos_alloc_test(void) {
    extern int write(int fd, const void *buf, unsigned int n);
    write(1, "[bisect:alloc-pre]", 18);
    unsigned int regs[8] = {0};
    regs[0] = 0x4800; regs[1] = 1;
    pktdrv_int_invoke(0x21, regs);
    write(1, "[bisect:alloc-post]", 19);
    return MP_OBJ_NEW_SMALL_INT(0);
}
static MP_DEFINE_CONST_FUN_OBJ_0(
    mod_uc386_net_dos_alloc_test_obj, mod_uc386_net_dos_alloc_test);

// Same as int60_then_alloc but with a HUGE local stack frame.
// If this hangs at INT 0x21 but the small-frame version works,
// stack size or alignment is the trigger.
static mp_obj_t mod_uc386_net_int60_then_alloc(void) {
    extern int write(int fd, const void *buf, unsigned int n);
    char big_local[400];           // bloat frame
    big_local[0] = 0;              // touch it so compiler keeps it
    write(1, "[bi:60pre]", 10);
    unsigned int r1[8] = {0};
    r1[0] = 0x0100; r1[1] = 0xFFFF;
    pktdrv_int_invoke(0x60, r1);
    write(1, "[bi:60post]", 11);
    write(1, "[bi:21pre]", 10);
    unsigned int r2[8] = {0};
    r2[0] = 0x4800; r2[1] = 1;
    pktdrv_int_invoke(0x21, r2);
    write(1, "[bi:21post]", 11);
    return MP_OBJ_NEW_SMALL_INT(big_local[0]);   // use it
}
static MP_DEFINE_CONST_FUN_OBJ_0(
    mod_uc386_net_int60_then_alloc_obj, mod_uc386_net_int60_then_alloc);

static mp_obj_t mod_uc386_net_pktdrv_diag(void) {
    mp_obj_t items[6];
    items[0] = MP_OBJ_NEW_SMALL_INT(pktdrv_thunk_invocations);
    items[1] = MP_OBJ_NEW_SMALL_INT(pktdrv_thunk_phase0_count);
    items[2] = MP_OBJ_NEW_SMALL_INT(pktdrv_thunk_phase1_count);
    items[3] = MP_OBJ_NEW_SMALL_INT(pktdrv_last_handle);
    items[4] = MP_OBJ_NEW_SMALL_INT(pktdrv_dpmi_seg);
    items[5] = MP_OBJ_NEW_SMALL_INT(pktdrv_dpmi_off);
    return mp_obj_new_tuple(6, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(
    mod_uc386_net_pktdrv_diag_obj, mod_uc386_net_pktdrv_diag);

static const mp_rom_map_elem_t mp_module_uc386_net_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),   MP_ROM_QSTR(MP_QSTR_uc386_net) },
    { MP_ROM_QSTR(MP_QSTR_eth_init),   MP_ROM_PTR(&mod_uc386_net_eth_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_eth_status), MP_ROM_PTR(&mod_uc386_net_eth_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_eth_set_static), MP_ROM_PTR(&mod_uc386_net_eth_set_static_obj) },
    { MP_ROM_QSTR(MP_QSTR_pktdrv_diag), MP_ROM_PTR(&mod_uc386_net_pktdrv_diag_obj) },
    { MP_ROM_QSTR(MP_QSTR_dos_alloc_test), MP_ROM_PTR(&mod_uc386_net_dos_alloc_test_obj) },
    { MP_ROM_QSTR(MP_QSTR_int60_then_alloc), MP_ROM_PTR(&mod_uc386_net_int60_then_alloc_obj) },
};
static MP_DEFINE_CONST_DICT(
    mp_module_uc386_net_globals, mp_module_uc386_net_globals_table);

const mp_obj_module_t mp_module_uc386_net = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_uc386_net_globals,
};
