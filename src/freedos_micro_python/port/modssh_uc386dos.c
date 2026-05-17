/* MicroPython _ssh module — thin wrapper around libssh2 for the
 * uc386-dos port.
 *
 * Status: WIP. Exposes:
 *   - module-level helpers: _ssh.version(), _ssh.crypto_engine(),
 *     _ssh.crypto_engine_name()
 *   - Session(socket) — performs handshake on the underlying TCP
 *     socket via libssh2's RECV/SEND callbacks routed to MP's
 *     stream protocol.
 *   - session.userauth_password(user, password)
 *   - session.exec(command) — opens a channel, exec's the command,
 *     drains stdout, closes; returns the captured bytes.
 *   - session.close()
 *
 * Restricts the negotiated host-key algorithm to ssh-ed25519 so the
 * axtls/TweetNaCl backend's verify path is used (RSA host-key verify
 * isn't wired yet). KEX is curve25519-sha256.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "py/runtime.h"
#include "py/stream.h"
#include "py/objstr.h"
#include "py/binary.h"

#include "libssh2.h"

/* ---------- libssh2 init guard ---------- */
static bool _ssh_initialized = false;
static void _ssh_lazy_init(void) {
    if (_ssh_initialized) return;
    libssh2_init(0);
    _ssh_initialized = true;
}

/* ---------- I/O callbacks: route libssh2 send/recv to MP socket ----------
 *
 * libssh2 calls these for every byte on the wire. We stash the MP
 * socket object pointer in the session's abstract slot at construction
 * time; the callbacks dereference it and hand the bytes to the
 * stream protocol's read/write methods (which on the lwIP socket
 * type land in lwip_tcp_send / lwip_tcp_receive). Blocking mode: a
 * recv call that wants N bytes blocks inside lwip_tcp_receive until
 * at least 1 byte is available; libssh2 loops calling recv until it
 * has the full record header / body it asked for.
 */

static ssize_t _ssh_recv_cb(libssh2_socket_t fd, void *buf, size_t len,
                            int flags, void **abstract) {
    (void)fd;
    (void)flags;
    mp_obj_t sock = (mp_obj_t)(*abstract);
    if (sock == MP_OBJ_NULL) {
        return -1;
    }
    ssize_t n = mp_stream_posix_read((void *)MP_OBJ_TO_PTR(sock), buf, len);
    /* lwip TCP returns 0 on a clean peer-FIN. libssh2's transport
     * layer treats recv() == 0 as a SOCKET_RECV failure and bails
     * out — even when it has already parsed channel packets queued
     * in session->packets. Returning -EAGAIN here makes libssh2 fall
     * through to the queue-drain branch in _libssh2_channel_read,
     * which is the right thing when the server has sent
     * CHANNEL_DATA + EXIT_STATUS + EOF + CLOSE just before its TCP
     * FIN: the queued packets carry the eof/close state, so the
     * next channel_read after the drain returns 0 cleanly. */
    if (n == 0) {
        return -EAGAIN;
    }
    return n;
}

static ssize_t _ssh_send_cb(libssh2_socket_t fd, const void *buf, size_t len,
                            int flags, void **abstract) {
    (void)fd;
    (void)flags;
    mp_obj_t sock = (mp_obj_t)(*abstract);
    if (sock == MP_OBJ_NULL) {
        return -1;
    }
    return mp_stream_posix_write((void *)MP_OBJ_TO_PTR(sock), buf, len);
}

/* ---------- Session ---------- */

typedef struct _mp_obj_ssh_session_t {
    mp_obj_base_t base;
    LIBSSH2_SESSION *session;
    mp_obj_t sock;                  /* the underlying TCP socket */
    bool authenticated;
} mp_obj_ssh_session_t;

static const mp_obj_type_t ssh_session_type;

static MP_NORETURN void _ssh_raise_session_error(LIBSSH2_SESSION *session,
                                                  int err, const char *fallback) {
    char *errmsg = NULL;
    int errmsg_len = 0;
    libssh2_session_last_error(session, &errmsg, &errmsg_len, 0);
    if (errmsg != NULL && errmsg_len > 0) {
        mp_obj_t args[2] = {
            MP_OBJ_NEW_SMALL_INT(err),
            mp_obj_new_str(errmsg, (size_t)errmsg_len),
        };
        nlr_raise(mp_obj_exception_make_new(&mp_type_OSError, 2, 0, args));
    }
    mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("%s (%d)"), fallback, err);
}

static mp_obj_t ssh_session_make_new(const mp_obj_type_t *type, size_t n_args,
                                       size_t n_kw, const mp_obj_t *args) {
    extern int write(int fd, const void *buf, unsigned int n);
    write(1, "[s:0]", 5);
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    _ssh_lazy_init();
    write(1, "[s:1]", 5);

    mp_obj_t sock = args[0];
    mp_get_stream_raise(sock, MP_STREAM_OP_READ | MP_STREAM_OP_WRITE);
    write(1, "[s:2]", 5);

    mp_obj_ssh_session_t *self = mp_obj_malloc(mp_obj_ssh_session_t, type);
    self->session = NULL;
    self->sock = sock;
    self->authenticated = false;

    self->session = libssh2_session_init();
    if (self->session == NULL) {
        mp_raise_OSError(MP_ENOMEM);
    }
    write(1, "[s:3]", 5);

    void **abstract = libssh2_session_abstract(self->session);
    *abstract = (void *)sock;

    libssh2_session_callback_set(self->session, LIBSSH2_CALLBACK_RECV,
                                  (void *)_ssh_recv_cb);
    libssh2_session_callback_set(self->session, LIBSSH2_CALLBACK_SEND,
                                  (void *)_ssh_send_cb);
    write(1, "[s:4]", 5);

    /* Lock the host-key algorithm to ssh-ed25519. */
    int r = libssh2_session_method_pref(self->session,
                                          LIBSSH2_METHOD_HOSTKEY,
                                          "ssh-ed25519");
    write(1, "[s:5]", 5);
    if (r != 0) {
        LIBSSH2_SESSION *s = self->session;
        self->session = NULL;
        _ssh_raise_session_error(s, r, "session_method_pref hostkey");
    }

    r = libssh2_session_method_pref(self->session,
                                     LIBSSH2_METHOD_KEX,
                                     "curve25519-sha256,curve25519-sha256@libssh.org");
    write(1, "[s:6]", 5);
    if (r != 0) {
        LIBSSH2_SESSION *s = self->session;
        self->session = NULL;
        _ssh_raise_session_error(s, r, "session_method_pref kex");
    }

    const char *ciphers = "aes256-ctr,aes128-ctr";
    r = libssh2_session_method_pref(self->session,
                                     LIBSSH2_METHOD_CRYPT_CS, ciphers);
    if (r == 0) {
        r = libssh2_session_method_pref(self->session,
                                         LIBSSH2_METHOD_CRYPT_SC, ciphers);
    }
    write(1, "[s:7]", 5);
    if (r != 0) {
        LIBSSH2_SESSION *s = self->session;
        self->session = NULL;
        _ssh_raise_session_error(s, r, "session_method_pref crypt");
    }
    const char *macs = "hmac-sha2-256,hmac-sha1";
    r = libssh2_session_method_pref(self->session,
                                     LIBSSH2_METHOD_MAC_CS, macs);
    if (r == 0) {
        r = libssh2_session_method_pref(self->session,
                                         LIBSSH2_METHOD_MAC_SC, macs);
    }
    write(1, "[s:8]", 5);
    if (r != 0) {
        LIBSSH2_SESSION *s = self->session;
        self->session = NULL;
        _ssh_raise_session_error(s, r, "session_method_pref mac");
    }

    libssh2_session_set_blocking(self->session, 1);
    write(1, "[s:9]", 5);

    r = libssh2_session_handshake(self->session, (libssh2_socket_t)0);
    write(1, "[s:A]", 5);
    if (r != 0) {
        LIBSSH2_SESSION *s = self->session;
        self->session = NULL;
        _ssh_raise_session_error(s, r, "session_handshake");
    }
    write(1, "[s:Z]", 5);
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t ssh_session_userauth_password(mp_obj_t self_in,
                                                mp_obj_t user_in,
                                                mp_obj_t pw_in) {
    mp_obj_ssh_session_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->session == NULL) {
        mp_raise_OSError(MP_EBADF);
    }
    size_t user_len, pw_len;
    const char *user = mp_obj_str_get_data(user_in, &user_len);
    const char *pw   = mp_obj_str_get_data(pw_in,   &pw_len);
    int r = libssh2_userauth_password_ex(self->session,
                                          user, (unsigned int)user_len,
                                          pw,   (unsigned int)pw_len,
                                          NULL);
    if (r != 0) {
        _ssh_raise_session_error(self->session, r, "userauth_password");
    }
    self->authenticated = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(ssh_session_userauth_password_obj,
                                  ssh_session_userauth_password);

/* session.exec(command) — open a fresh channel, exec the command,
 * drain stdout until EOF, close. Returns the captured bytes.
 * Minimum-viable exec channel: no stdin, no stderr capture, no
 * exit-status reporting. */
static mp_obj_t ssh_session_exec(mp_obj_t self_in, mp_obj_t cmd_in) {
    mp_obj_ssh_session_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->session == NULL) {
        mp_raise_OSError(MP_EBADF);
    }
    size_t cmd_len;
    const char *cmd = mp_obj_str_get_data(cmd_in, &cmd_len);

    LIBSSH2_CHANNEL *channel = libssh2_channel_open_session(self->session);
    if (channel == NULL) {
        _ssh_raise_session_error(self->session, -1, "channel_open_session");
    }
    int r = libssh2_channel_exec(channel, cmd);
    if (r != 0) {
        libssh2_channel_free(channel);
        _ssh_raise_session_error(self->session, r, "channel_exec");
    }

    vstr_t vstr;
    vstr_init(&vstr, 256);
    char buf[256];
    for (;;) {
        ssize_t n = libssh2_channel_read(channel, buf, sizeof(buf));
        if (n > 0) {
            vstr_add_strn(&vstr, buf, (size_t)n);
        } else if (n == 0) {
            /* EOF on stdout */
            if (libssh2_channel_eof(channel)) {
                break;
            }
            /* No data right now but stream still open — keep polling. */
        } else {
            /* Negative = error or LIBSSH2_ERROR_EAGAIN.
             * After server-side `echo ...; exit` the server sends
             * CHANNEL_DATA + EXIT_STATUS + EOF + CLOSE and then
             * shuts down its TCP write side. Our next recv() sees
             * EOF; libssh2 reports LIBSSH2_ERROR_SOCKET_RECV (-43)
             * or LIBSSH2_ERROR_SOCKET_DISCONNECT (-32). Treat as
             * end-of-stream once any data has been captured; bail
             * only if we never got anything. */
            if (vstr_len(&vstr) > 0 ||
                libssh2_channel_eof(channel) ||
                (int)n == LIBSSH2_ERROR_SOCKET_RECV ||
                (int)n == LIBSSH2_ERROR_SOCKET_DISCONNECT) {
                break;
            }
            vstr_clear(&vstr);
            libssh2_channel_free(channel);
            _ssh_raise_session_error(self->session, (int)n, "channel_read");
        }
    }
    /* Best-effort close; transport may already be torn down. */
    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ssh_session_exec_obj, ssh_session_exec);

static mp_obj_t ssh_session_close(mp_obj_t self_in) {
    mp_obj_ssh_session_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->session != NULL) {
        libssh2_session_disconnect(self->session, "bye");
        libssh2_session_free(self->session);
        self->session = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ssh_session_close_obj, ssh_session_close);

static const mp_rom_map_elem_t ssh_session_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_userauth_password),
        MP_ROM_PTR(&ssh_session_userauth_password_obj) },
    { MP_ROM_QSTR(MP_QSTR_exec),  MP_ROM_PTR(&ssh_session_exec_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&ssh_session_close_obj) },
};
static MP_DEFINE_CONST_DICT(ssh_session_locals_dict,
                             ssh_session_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    ssh_session_type,
    MP_QSTR_Session,
    MP_TYPE_FLAG_NONE,
    make_new, ssh_session_make_new,
    locals_dict, &ssh_session_locals_dict
);

/* ---------- module-level helpers ---------- */

static mp_obj_t mod_ssh_version(void) {
    const char *v = libssh2_version(0);
    if (v == NULL) {
        return mp_const_none;
    }
    return mp_obj_new_str(v, strlen(v));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_ssh_version_obj, mod_ssh_version);

static mp_obj_t mod_ssh_crypto_engine(void) {
    return MP_OBJ_NEW_SMALL_INT(libssh2_crypto_engine());
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_ssh_crypto_engine_obj, mod_ssh_crypto_engine);

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
static MP_DEFINE_CONST_FUN_OBJ_0(mod_ssh_crypto_engine_name_obj,
                                  mod_ssh_crypto_engine_name);

static const mp_rom_map_elem_t mod_ssh_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),           MP_ROM_QSTR(MP_QSTR__ssh) },
    { MP_ROM_QSTR(MP_QSTR_version),            MP_ROM_PTR(&mod_ssh_version_obj) },
    { MP_ROM_QSTR(MP_QSTR_crypto_engine),      MP_ROM_PTR(&mod_ssh_crypto_engine_obj) },
    { MP_ROM_QSTR(MP_QSTR_crypto_engine_name), MP_ROM_PTR(&mod_ssh_crypto_engine_name_obj) },
    { MP_ROM_QSTR(MP_QSTR_Session),            MP_ROM_PTR(&ssh_session_type) },
};
static MP_DEFINE_CONST_DICT(mod_ssh_globals, mod_ssh_globals_table);

const mp_obj_module_t mp_module_ssh = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mod_ssh_globals,
};
