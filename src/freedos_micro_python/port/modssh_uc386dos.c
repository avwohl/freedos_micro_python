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
#include <sys/stat.h>  /* struct stat — used by libssh2_struct_stat */

#include "py/runtime.h"
#include "py/stream.h"
#include "py/objstr.h"
#include "py/binary.h"

#include "libssh2.h"
#include "libssh2_sftp.h"

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
    /* Same treatment for ETIMEDOUT (MP_ETIMEDOUT = 110): when the
     * rig sets a short socket timeout to bound libssh2's
     * `channel_write` drain-loop (which would otherwise block
     * forever waiting for incoming packets that won't come), the
     * recv returns -1 with errno = 110. Map to -EAGAIN so libssh2's
     * drain loop and BLOCK_ADJUST treat it as "no data right now"
     * and keep retrying instead of tearing the session down.
     * uc386's libc doesn't expose ETIMEDOUT as a constant — we
     * spell out the MP value (matches modlwip.c). */
    if (n < 0 && errno == 110) {
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

/* ---------- SFTP ---------- */

typedef struct _mp_obj_ssh_sftp_t {
    mp_obj_base_t base;
    LIBSSH2_SFTP *sftp;
    mp_obj_t session;   /* keep parent Session alive while sftp is open */
} mp_obj_ssh_sftp_t;

typedef struct _mp_obj_ssh_sftp_file_t {
    mp_obj_base_t base;
    LIBSSH2_SFTP_HANDLE *handle;
    mp_obj_t sftp;      /* keep SFTP alive while a handle is open */
} mp_obj_ssh_sftp_file_t;

static const mp_obj_type_t ssh_sftp_type;
static const mp_obj_type_t ssh_sftp_file_type;

static mp_obj_t ssh_sftp_file_close(mp_obj_t self_in) {
    mp_obj_ssh_sftp_file_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle != NULL) {
        libssh2_sftp_close_handle(self->handle);
        self->handle = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ssh_sftp_file_close_obj, ssh_sftp_file_close);

static mp_obj_t ssh_sftp_file_read(size_t n_args, const mp_obj_t *args) {
    mp_obj_ssh_sftp_file_t *self = MP_OBJ_TO_PTR(args[0]);
    if (self->handle == NULL) {
        mp_raise_OSError(MP_EBADF);
    }
    mp_obj_ssh_sftp_t *sftp = MP_OBJ_TO_PTR(self->sftp);
    mp_obj_ssh_session_t *sess =
        MP_OBJ_TO_PTR(((mp_obj_ssh_sftp_t *)sftp)->session);
    /* If no size given, read until EOF. */
    mp_int_t max_len = (n_args >= 2) ? mp_obj_get_int(args[1]) : -1;
    vstr_t vstr;
    vstr_init(&vstr, 1024);
    char buf[1024];
    for (;;) {
        size_t want = (max_len < 0 || (mp_int_t)sizeof(buf) <= max_len)
                          ? sizeof(buf)
                          : (size_t)max_len;
        ssize_t n = libssh2_sftp_read(self->handle, buf, want);
        if (n > 0) {
            vstr_add_strn(&vstr, buf, (size_t)n);
            if (max_len > 0) {
                max_len -= n;
                if (max_len == 0) break;
            }
        } else if (n == 0) {
            break;  /* EOF */
        } else {
            vstr_clear(&vstr);
            _ssh_raise_session_error(sess->session, (int)n, "sftp_read");
        }
    }
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ssh_sftp_file_read_obj, 1, 2,
                                             ssh_sftp_file_read);

static mp_obj_t ssh_sftp_file_write(mp_obj_t self_in, mp_obj_t data_in) {
    mp_obj_ssh_sftp_file_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle == NULL) {
        mp_raise_OSError(MP_EBADF);
    }
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);
    const char *p = (const char *)bufinfo.buf;
    size_t remaining = bufinfo.len;
    mp_obj_ssh_sftp_t *sftp = MP_OBJ_TO_PTR(self->sftp);
    mp_obj_ssh_session_t *sess = MP_OBJ_TO_PTR(sftp->session);
    while (remaining > 0) {
        ssize_t n = libssh2_sftp_write(self->handle, p, remaining);
        if (n < 0) {
            _ssh_raise_session_error(sess->session, (int)n, "sftp_write");
        }
        if (n == 0) break;
        p += n;
        remaining -= (size_t)n;
    }
    return MP_OBJ_NEW_SMALL_INT(bufinfo.len - remaining);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ssh_sftp_file_write_obj, ssh_sftp_file_write);

static const mp_rom_map_elem_t ssh_sftp_file_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read),  MP_ROM_PTR(&ssh_sftp_file_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&ssh_sftp_file_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&ssh_sftp_file_close_obj) },
};
static MP_DEFINE_CONST_DICT(ssh_sftp_file_locals_dict,
                             ssh_sftp_file_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    ssh_sftp_file_type,
    MP_QSTR_SFTPFile,
    MP_TYPE_FLAG_NONE,
    locals_dict, &ssh_sftp_file_locals_dict
);

static mp_obj_t ssh_sftp_open(size_t n_args, const mp_obj_t *args) {
    mp_obj_ssh_sftp_t *self = MP_OBJ_TO_PTR(args[0]);
    if (self->sftp == NULL) {
        mp_raise_OSError(MP_EBADF);
    }
    const char *path = mp_obj_str_get_str(args[1]);
    const char *mode = (n_args >= 3) ? mp_obj_str_get_str(args[2]) : "r";
    /* Mode mapping: 'r' = read; 'w' = write+create+truncate. */
    unsigned long flags;
    long fmode = 0644;
    if (mode[0] == 'w') {
        flags = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC;
    } else {
        flags = LIBSSH2_FXF_READ;
    }
    LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open(self->sftp, path, flags, fmode);
    if (h == NULL) {
        mp_obj_ssh_session_t *sess = MP_OBJ_TO_PTR(self->session);
        _ssh_raise_session_error(sess->session, -1, "sftp_open");
    }
    mp_obj_ssh_sftp_file_t *fobj = mp_obj_malloc(mp_obj_ssh_sftp_file_t,
                                                  &ssh_sftp_file_type);
    fobj->handle = h;
    fobj->sftp = MP_OBJ_FROM_PTR(self);
    return MP_OBJ_FROM_PTR(fobj);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ssh_sftp_open_obj, 2, 3,
                                             ssh_sftp_open);

static mp_obj_t ssh_sftp_close(mp_obj_t self_in) {
    mp_obj_ssh_sftp_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->sftp != NULL) {
        libssh2_sftp_shutdown(self->sftp);
        self->sftp = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ssh_sftp_close_obj, ssh_sftp_close);

/* ---------- SFTPDir (directory handle, returned by SFTP.opendir) ---------- */
typedef struct _mp_obj_ssh_sftp_dir_t {
    mp_obj_base_t base;
    LIBSSH2_SFTP_HANDLE *handle;
    mp_obj_t sftp;
} mp_obj_ssh_sftp_dir_t;
static const mp_obj_type_t ssh_sftp_dir_type;

static mp_obj_t _attrs_to_tuple(const LIBSSH2_SFTP_ATTRIBUTES *a) {
    /* (mode, size, atime, mtime, uid, gid) — fields not present in
     * attrs->flags read as 0. */
    mp_obj_t t[6];
    t[0] = mp_obj_new_int((a->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                           ? (mp_int_t)a->permissions : 0);
    t[1] = mp_obj_new_int_from_ull((a->flags & LIBSSH2_SFTP_ATTR_SIZE)
                                    ? a->filesize : 0);
    t[2] = mp_obj_new_int((a->flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
                           ? (mp_int_t)a->atime : 0);
    t[3] = mp_obj_new_int((a->flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
                           ? (mp_int_t)a->mtime : 0);
    t[4] = mp_obj_new_int((a->flags & LIBSSH2_SFTP_ATTR_UIDGID)
                           ? (mp_int_t)a->uid : 0);
    t[5] = mp_obj_new_int((a->flags & LIBSSH2_SFTP_ATTR_UIDGID)
                           ? (mp_int_t)a->gid : 0);
    return mp_obj_new_tuple(6, t);
}

static mp_obj_t ssh_sftp_dir_read(mp_obj_t self_in) {
    mp_obj_ssh_sftp_dir_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle == NULL) {
        mp_raise_OSError(MP_EBADF);
    }
    char name[256];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    int rc = libssh2_sftp_readdir(self->handle, name, sizeof(name), &attrs);
    if (rc == 0) {
        return mp_const_none;  /* end of directory */
    }
    if (rc < 0) {
        mp_obj_ssh_sftp_t *sftp = MP_OBJ_TO_PTR(self->sftp);
        mp_obj_ssh_session_t *sess = MP_OBJ_TO_PTR(sftp->session);
        _ssh_raise_session_error(sess->session, rc, "sftp_readdir");
    }
    mp_obj_t pair[2];
    pair[0] = mp_obj_new_str(name, (size_t)rc);
    pair[1] = _attrs_to_tuple(&attrs);
    return mp_obj_new_tuple(2, pair);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ssh_sftp_dir_read_obj, ssh_sftp_dir_read);

static mp_obj_t ssh_sftp_dir_close(mp_obj_t self_in) {
    mp_obj_ssh_sftp_dir_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle != NULL) {
        libssh2_sftp_closedir(self->handle);
        self->handle = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ssh_sftp_dir_close_obj, ssh_sftp_dir_close);

static const mp_rom_map_elem_t ssh_sftp_dir_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read),  MP_ROM_PTR(&ssh_sftp_dir_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&ssh_sftp_dir_close_obj) },
};
static MP_DEFINE_CONST_DICT(ssh_sftp_dir_locals_dict,
                             ssh_sftp_dir_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    ssh_sftp_dir_type,
    MP_QSTR_SFTPDir,
    MP_TYPE_FLAG_NONE,
    locals_dict, &ssh_sftp_dir_locals_dict
);

/* ---------- SFTP filesystem-op methods ---------- */

static mp_obj_t ssh_sftp_opendir(mp_obj_t self_in, mp_obj_t path_in) {
    mp_obj_ssh_sftp_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->sftp == NULL) {
        mp_raise_OSError(MP_EBADF);
    }
    const char *path = mp_obj_str_get_str(path_in);
    LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_opendir(self->sftp, path);
    if (h == NULL) {
        mp_obj_ssh_session_t *sess = MP_OBJ_TO_PTR(self->session);
        _ssh_raise_session_error(sess->session, -1, "sftp_opendir");
    }
    mp_obj_ssh_sftp_dir_t *d = mp_obj_malloc(mp_obj_ssh_sftp_dir_t,
                                              &ssh_sftp_dir_type);
    d->handle = h;
    d->sftp = self_in;
    return MP_OBJ_FROM_PTR(d);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ssh_sftp_opendir_obj, ssh_sftp_opendir);

static mp_obj_t ssh_sftp_mkdir(size_t n_args, const mp_obj_t *args) {
    mp_obj_ssh_sftp_t *self = MP_OBJ_TO_PTR(args[0]);
    if (self->sftp == NULL) {
        mp_raise_OSError(MP_EBADF);
    }
    const char *path = mp_obj_str_get_str(args[1]);
    long mode = (n_args == 3) ? mp_obj_get_int(args[2]) : 0755;
    int rc = libssh2_sftp_mkdir(self->sftp, path, mode);
    if (rc != 0) {
        mp_obj_ssh_session_t *sess = MP_OBJ_TO_PTR(self->session);
        _ssh_raise_session_error(sess->session, rc, "sftp_mkdir");
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ssh_sftp_mkdir_obj, 2, 3,
                                             ssh_sftp_mkdir);

static mp_obj_t ssh_sftp_rmdir(mp_obj_t self_in, mp_obj_t path_in) {
    mp_obj_ssh_sftp_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->sftp == NULL) {
        mp_raise_OSError(MP_EBADF);
    }
    int rc = libssh2_sftp_rmdir(self->sftp, mp_obj_str_get_str(path_in));
    if (rc != 0) {
        mp_obj_ssh_session_t *sess = MP_OBJ_TO_PTR(self->session);
        _ssh_raise_session_error(sess->session, rc, "sftp_rmdir");
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ssh_sftp_rmdir_obj, ssh_sftp_rmdir);

static mp_obj_t ssh_sftp_unlink(mp_obj_t self_in, mp_obj_t path_in) {
    mp_obj_ssh_sftp_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->sftp == NULL) {
        mp_raise_OSError(MP_EBADF);
    }
    int rc = libssh2_sftp_unlink(self->sftp, mp_obj_str_get_str(path_in));
    if (rc != 0) {
        mp_obj_ssh_session_t *sess = MP_OBJ_TO_PTR(self->session);
        _ssh_raise_session_error(sess->session, rc, "sftp_unlink");
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ssh_sftp_unlink_obj, ssh_sftp_unlink);

static mp_obj_t ssh_sftp_rename(mp_obj_t self_in, mp_obj_t old_in,
                                  mp_obj_t new_in) {
    mp_obj_ssh_sftp_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->sftp == NULL) {
        mp_raise_OSError(MP_EBADF);
    }
    const char *old_path = mp_obj_str_get_str(old_in);
    const char *new_path = mp_obj_str_get_str(new_in);
    int rc = libssh2_sftp_rename(self->sftp, old_path, new_path);
    if (rc != 0) {
        mp_obj_ssh_session_t *sess = MP_OBJ_TO_PTR(self->session);
        _ssh_raise_session_error(sess->session, rc, "sftp_rename");
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(ssh_sftp_rename_obj, ssh_sftp_rename);

static mp_obj_t ssh_sftp_stat(mp_obj_t self_in, mp_obj_t path_in) {
    mp_obj_ssh_sftp_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->sftp == NULL) {
        mp_raise_OSError(MP_EBADF);
    }
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    int rc = libssh2_sftp_stat(self->sftp, mp_obj_str_get_str(path_in), &attrs);
    if (rc != 0) {
        mp_obj_ssh_session_t *sess = MP_OBJ_TO_PTR(self->session);
        _ssh_raise_session_error(sess->session, rc, "sftp_stat");
    }
    return _attrs_to_tuple(&attrs);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ssh_sftp_stat_obj, ssh_sftp_stat);

static mp_obj_t ssh_sftp_realpath(mp_obj_t self_in, mp_obj_t path_in) {
    mp_obj_ssh_sftp_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->sftp == NULL) {
        mp_raise_OSError(MP_EBADF);
    }
    char target[512];
    int rc = libssh2_sftp_realpath(self->sftp, mp_obj_str_get_str(path_in),
                                     target, sizeof(target));
    if (rc < 0) {
        mp_obj_ssh_session_t *sess = MP_OBJ_TO_PTR(self->session);
        _ssh_raise_session_error(sess->session, rc, "sftp_realpath");
    }
    return mp_obj_new_str(target, (size_t)rc);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ssh_sftp_realpath_obj, ssh_sftp_realpath);

static const mp_rom_map_elem_t ssh_sftp_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_open),     MP_ROM_PTR(&ssh_sftp_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_opendir),  MP_ROM_PTR(&ssh_sftp_opendir_obj) },
    { MP_ROM_QSTR(MP_QSTR_mkdir),    MP_ROM_PTR(&ssh_sftp_mkdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_rmdir),    MP_ROM_PTR(&ssh_sftp_rmdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_unlink),   MP_ROM_PTR(&ssh_sftp_unlink_obj) },
    { MP_ROM_QSTR(MP_QSTR_rename),   MP_ROM_PTR(&ssh_sftp_rename_obj) },
    { MP_ROM_QSTR(MP_QSTR_stat),     MP_ROM_PTR(&ssh_sftp_stat_obj) },
    { MP_ROM_QSTR(MP_QSTR_realpath), MP_ROM_PTR(&ssh_sftp_realpath_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),    MP_ROM_PTR(&ssh_sftp_close_obj) },
};
static MP_DEFINE_CONST_DICT(ssh_sftp_locals_dict,
                             ssh_sftp_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    ssh_sftp_type,
    MP_QSTR_SFTP,
    MP_TYPE_FLAG_NONE,
    locals_dict, &ssh_sftp_locals_dict
);

static mp_obj_t ssh_session_sftp(mp_obj_t self_in) {
    mp_obj_ssh_session_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->session == NULL) {
        mp_raise_OSError(MP_EBADF);
    }
    LIBSSH2_SFTP *sftp = libssh2_sftp_init(self->session);
    if (sftp == NULL) {
        _ssh_raise_session_error(self->session, -1, "sftp_init");
    }
    mp_obj_ssh_sftp_t *sftp_obj = mp_obj_malloc(mp_obj_ssh_sftp_t,
                                                  &ssh_sftp_type);
    sftp_obj->sftp = sftp;
    sftp_obj->session = self_in;
    return MP_OBJ_FROM_PTR(sftp_obj);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ssh_session_sftp_obj, ssh_session_sftp);

/* ---------- SCP ----------
 *
 * libssh2's SCP API is one-shot: each call opens a channel, runs `scp -t`
 * or `scp -f` on the server, exchanges the SCP protocol (length-prefixed
 * binary), and tears the channel down. No persistent SCP object — keep
 * the wrappers as session methods returning bytes / int.
 */

/* session.scp_recv(path) — download a file, return its bytes. */
static mp_obj_t ssh_session_scp_recv(mp_obj_t self_in, mp_obj_t path_in) {
    mp_obj_ssh_session_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->session == NULL) {
        mp_raise_OSError(MP_EBADF);
    }
    const char *path = mp_obj_str_get_str(path_in);
    /* Pass NULL for `sb`: libssh2 emits `scp -f path` (without `-p`),
     * so the server doesn't need to send a `T<mtime> 0 <atime> 0\n`
     * time-info line. We surface neither size nor mtime/atime to
     * Python, so the sb output isn't useful anyway. Drawback: no
     * upfront file size, so we accumulate bytes until either the
     * server's CHANNEL_EOF flips libssh2_channel_eof() to true, or
     * we time out via the iteration cap (server didn't close —
     * common with paramiko if the SSH transport teared down before
     * channel.close() ran). */
    LIBSSH2_CHANNEL *channel = libssh2_scp_recv2(self->session, path, NULL);
    if (channel == NULL) {
        _ssh_raise_session_error(self->session, -1, "scp_recv2");
    }
    vstr_t vstr;
    vstr_init(&vstr, 256);
    char buf[256];
    int empty_polls = 0;
    for (;;) {
        ssize_t n = libssh2_channel_read(channel, buf, sizeof(buf));
        if (n > 0) {
            vstr_add_strn(&vstr, buf, (size_t)n);
            empty_polls = 0;
            if (libssh2_channel_eof(channel)) {
                break;
            }
        } else if (n == 0) {
            break;
        } else if ((int)n == LIBSSH2_ERROR_EAGAIN) {
            /* libssh2_channel_read already waited via BLOCK_ADJUST →
             * our wait_socket shim. If we hit EAGAIN repeatedly after
             * receiving some bytes — and CHANNEL_EOF still hasn't
             * arrived — the server isn't sending more. Bail with what
             * we have. paramiko's transport sometimes drops the
             * CHANNEL_EOF after the SCP exec finishes, which would
             * otherwise spin here forever. */
            if (vstr_len(&vstr) > 0 && ++empty_polls > 20) {
                break;
            }
            continue;
        } else if ((int)n == LIBSSH2_ERROR_SOCKET_RECV ||
                   (int)n == LIBSSH2_ERROR_SOCKET_DISCONNECT) {
            break;
        } else {
            vstr_clear(&vstr);
            libssh2_channel_free(channel);
            _ssh_raise_session_error(self->session, (int)n, "scp_channel_read");
        }
    }
    /* Strip the SCP trailing \0 terminator if present (server appends
     * \0 after the file payload as the SCP end-of-data marker). */
    if (vstr_len(&vstr) > 0 && vstr.buf[vstr_len(&vstr) - 1] == '\0') {
        vstr.len--;
    }
    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ssh_session_scp_recv_obj, ssh_session_scp_recv);

/* session.scp_send(path, mode, data) — upload data to path with the given
 * unix mode (e.g. 0o644). Returns number of bytes written. */
static mp_obj_t ssh_session_scp_send(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    mp_obj_ssh_session_t *self = MP_OBJ_TO_PTR(args[0]);
    if (self->session == NULL) {
        mp_raise_OSError(MP_EBADF);
    }
    const char *path = mp_obj_str_get_str(args[1]);
    int mode = mp_obj_get_int(args[2]);
    mp_buffer_info_t bi;
    mp_get_buffer_raise(args[3], &bi, MP_BUFFER_READ);
    LIBSSH2_CHANNEL *channel = libssh2_scp_send_ex(self->session, path, mode,
                                                    (libssh2_int64_t)bi.len,
                                                    0, 0);
    if (channel == NULL) {
        _ssh_raise_session_error(self->session, -1, "scp_send_ex");
    }
    const char *p = (const char *)bi.buf;
    size_t remaining = bi.len;
    while (remaining > 0) {
        ssize_t n = libssh2_channel_write(channel, p, remaining);
        if ((int)n == LIBSSH2_ERROR_EAGAIN) {
            continue;
        }
        if (n < 0) {
            libssh2_channel_free(channel);
            _ssh_raise_session_error(self->session, (int)n, "scp_channel_write");
        }
        p += n;
        remaining -= (size_t)n;
    }
    /* SCP protocol: send the trailing \0 terminator after the file
     * payload. Some servers gate persistence on receiving this byte
     * (libssh2's own scp_recv does), so omitting it leaves the
     * server-side file empty even though every data byte was sent.
     * We don't wait for the server's final-ack \0 — the server
     * tears the channel down after persisting, and `_libssh2_wait_socket`
     * would just spin. */
    {
        const char nul = '\0';
        for (;;) {
            ssize_t n = libssh2_channel_write(channel, &nul, 1);
            if (n == 1) break;
            if ((int)n != LIBSSH2_ERROR_EAGAIN) break;
        }
    }
    libssh2_channel_send_eof(channel);
    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
    return mp_obj_new_int((mp_int_t)bi.len);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ssh_session_scp_send_obj, 4, 4,
                                             ssh_session_scp_send);

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
    { MP_ROM_QSTR(MP_QSTR_sftp),  MP_ROM_PTR(&ssh_session_sftp_obj) },
    { MP_ROM_QSTR(MP_QSTR_scp_recv), MP_ROM_PTR(&ssh_session_scp_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_scp_send), MP_ROM_PTR(&ssh_session_scp_send_obj) },
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
