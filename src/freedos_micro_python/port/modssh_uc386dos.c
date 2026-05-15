/* MicroPython _ssh module — thin wrapper around libssh2 for the
 * uc386-dos port.
 *
 * Status: SKELETON. Exposes `_ssh.version()` and
 * `_ssh.crypto_engine()` so we can verify the libssh2 link surface
 * is wired into MP correctly. Session / channel / SFTP entry points
 * land in subsequent commits once the libssh2_axtls.c crypto
 * adapter's RSA / DH / key-parse stubs are filled in (currently
 * those return error so any real handshake fails).
 *
 * Once those land, the Python-level API at
 * `src/freedos_micro_python/examples/ssh.py` and `scp.py` will
 * wrap this module the same way `wget.py` wraps `socket` + `ssl`.
 */

#include "py/runtime.h"
#include "py/objstr.h"

#include "libssh2.h"

/* libssh2's libssh2_version() takes a min-required-version arg and
 * returns the actual version string only if the runtime version is
 * >= the request. Passing 0 always returns the string. */
static mp_obj_t mod_ssh_version(void) {
    const char *v = libssh2_version(0);
    if (v == NULL) {
        return mp_const_none;
    }
    return mp_obj_new_str(v, strlen(v));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_ssh_version_obj, mod_ssh_version);

/* libssh2_crypto_engine() returns the enum value identifying the
 * compiled-in backend. With LIBSSH2_AXTLS we get
 * `libssh2_axtls` (5). Surface it as the integer for now — Python
 * code can map to a string. */
static mp_obj_t mod_ssh_crypto_engine(void) {
    return MP_OBJ_NEW_SMALL_INT(libssh2_crypto_engine());
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_ssh_crypto_engine_obj, mod_ssh_crypto_engine);

/* Convenience: name of the crypto engine. Keeps Python code from
 * having to memorise the libssh2 enum encoding. */
static mp_obj_t mod_ssh_crypto_engine_name(void) {
    int e = libssh2_crypto_engine();
    const char *name;
    switch (e) {
        case libssh2_no_crypto:  name = "none";     break;
        case libssh2_openssl:    name = "openssl";  break;
        case libssh2_gcrypt:     name = "gcrypt";   break;
        case libssh2_mbedtls:    name = "mbedtls";  break;
        case libssh2_wincng:     name = "wincng";   break;
        case libssh2_os400qc3:   name = "os400qc3"; break;
        case libssh2_axtls:      name = "axtls";    break;
        default:                 name = "unknown";  break;
    }
    return mp_obj_new_str(name, strlen(name));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_ssh_crypto_engine_name_obj, mod_ssh_crypto_engine_name);

static const mp_rom_map_elem_t mod_ssh_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),           MP_ROM_QSTR(MP_QSTR__ssh) },
    { MP_ROM_QSTR(MP_QSTR_version),            MP_ROM_PTR(&mod_ssh_version_obj) },
    { MP_ROM_QSTR(MP_QSTR_crypto_engine),      MP_ROM_PTR(&mod_ssh_crypto_engine_obj) },
    { MP_ROM_QSTR(MP_QSTR_crypto_engine_name), MP_ROM_PTR(&mod_ssh_crypto_engine_name_obj) },
};
static MP_DEFINE_CONST_DICT(mod_ssh_globals, mod_ssh_globals_table);

const mp_obj_module_t mp_module_ssh = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mod_ssh_globals,
};
