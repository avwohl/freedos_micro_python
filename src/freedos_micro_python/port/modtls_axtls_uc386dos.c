/*
 * uc386-dos modtls — fork of upstream/extmod/modtls_axtls.c
 * (https://github.com/micropython/micropython, MIT) with the
 * CPython-compatible cert-verification surface that upstream's
 * minimal version omits:
 *
 *   - `verify_mode` is a real settable attribute (CERT_NONE /
 *     CERT_REQUIRED), backed by a field on the SSLContext.
 *   - `SSLContext.load_verify_locations(cadata=...)` loads one
 *     or more PEM-encoded CA certs into the context (axtls's
 *     SSL_OBJ_X509_CACERT slot). cadata accepts a bytes/str of
 *     PEM data; cafile/capath aren't supported (no VFS exposure
 *     to axtls).
 *   - When `verify_mode == CERT_REQUIRED`, the wrap_socket path
 *     drops the `SSL_SERVER_VERIFY_LATER` option so axtls fails
 *     the handshake on bad certs instead of silently accepting.
 *
 * Build-side: this file replaces upstream's modtls_axtls.c in the
 * source list. Both register `MP_QSTR_tls`, but only one TU
 * actually compiles thanks to the swap in build_port.sh.
 */

#include <stdio.h>
#include <string.h>

#include "py/runtime.h"
#include "py/stream.h"
#include "py/objstr.h"

#if MICROPY_PY_SSL && MICROPY_SSL_AXTLS

#include "ssl.h"

#define PROTOCOL_TLS_CLIENT (0)
#define PROTOCOL_TLS_SERVER (1)

#define CERT_NONE     (0)
#define CERT_REQUIRED (2)  // matches CPython ssl.CERT_REQUIRED

typedef struct _mp_obj_ssl_context_t {
    mp_obj_base_t base;
    mp_obj_t key;
    mp_obj_t cert;
    SSL_CTX *ssl_ctx;
    int verify_mode;
} mp_obj_ssl_context_t;

typedef struct _mp_obj_ssl_socket_t {
    mp_obj_base_t base;
    mp_obj_t sock;
    SSL_CTX *ssl_ctx;
    SSL *ssl_sock;
    byte *buf;
    uint32_t bytes_left;
    bool blocking;
} mp_obj_ssl_socket_t;

static const mp_obj_type_t ssl_context_type;
static const mp_obj_type_t ssl_socket_type;

static mp_obj_t ssl_socket_make_new(mp_obj_ssl_context_t *ssl_context, mp_obj_t sock,
    bool server_side, bool do_handshake_on_connect, mp_obj_t server_hostname);

// Error-string tables (verbatim from upstream).
static const char *const ssl_error_tab1[] = {
    "NOT_OK", "DEAD", "CLOSE_NOTIFY", "EAGAIN",
};
static const char *const ssl_error_tab2[] = {
    "CONN_LOST", "RECORD_OVERFLOW", "SOCK_SETUP_FAILURE", NULL,
    "INVALID_HANDSHAKE", "INVALID_PROT_MSG", "INVALID_HMAC",
    "INVALID_VERSION", "UNSUPPORTED_EXTENSION", "INVALID_SESSION",
    "NO_CIPHER", "INVALID_CERT_HASH_ALG", "BAD_CERTIFICATE",
    "INVALID_KEY", NULL, "FINISHED_INVALID", "NO_CERT_DEFINED",
    "NO_CLIENT_RENOG", "NOT_SUPPORTED",
};

static MP_NORETURN void ssl_raise_error(int err) {
    MP_STATIC_ASSERT(SSL_NOT_OK - 3 == SSL_EAGAIN);
    MP_STATIC_ASSERT(SSL_ERROR_CONN_LOST - 18 == SSL_ERROR_NOT_SUPPORTED);

    const char *errstr = NULL;
    if (SSL_NOT_OK >= err && err >= SSL_EAGAIN) {
        errstr = ssl_error_tab1[SSL_NOT_OK - err];
    } else if (SSL_ERROR_CONN_LOST >= err && err >= SSL_ERROR_NOT_SUPPORTED) {
        errstr = ssl_error_tab2[SSL_ERROR_CONN_LOST - err];
    }
    if (errstr == NULL) {
        mp_raise_OSError(err);
    }
    mp_obj_str_t *o_str = m_new_obj_maybe(mp_obj_str_t);
    if (o_str == NULL) {
        mp_raise_OSError(err);
    }
    o_str->base.type = &mp_type_str;
    o_str->data = (const byte *)errstr;
    o_str->len = strlen((char *)o_str->data);
    o_str->hash = qstr_compute_hash(o_str->data, o_str->len);

    mp_obj_t args[2] = { MP_OBJ_NEW_SMALL_INT(err), MP_OBJ_FROM_PTR(o_str) };
    nlr_raise(mp_obj_exception_make_new(&mp_type_OSError, 2, 0, args));
}

// SSLContext —————————————————————————————————————————————————

static mp_obj_t ssl_context_make_new(const mp_obj_type_t *type_in, size_t n_args,
                                     size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    // The `protocol` argument is accepted but ignored — axtls picks
    // its own version from CONFIG_SSL_PROT_LOW.

    #if MICROPY_PY_SSL_FINALISER
    mp_obj_ssl_context_t *self = mp_obj_malloc_with_finaliser(mp_obj_ssl_context_t, type_in);
    #else
    mp_obj_ssl_context_t *self = mp_obj_malloc(mp_obj_ssl_context_t, type_in);
    #endif
    self->key = mp_const_none;
    self->cert = mp_const_none;
    self->verify_mode = CERT_NONE;

    // We allocate the SSL_CTX up front so load_verify_locations /
    // load_cert_chain can stash data into it before any wrap_socket.
    // SSL_DEFAULT_CLNT_SESS = 1 (single session resumption slot).
    self->ssl_ctx = ssl_ctx_new(SSL_SERVER_VERIFY_LATER, SSL_DEFAULT_CLNT_SESS);
    if (self->ssl_ctx == NULL) {
        mp_raise_OSError(MP_ENOMEM);
    }

    return MP_OBJ_FROM_PTR(self);
}

static void ssl_context_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    mp_obj_ssl_context_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) {
        // Load.
        if (attr == MP_QSTR_verify_mode) {
            dest[0] = MP_OBJ_NEW_SMALL_INT(self->verify_mode);
        } else {
            dest[1] = MP_OBJ_SENTINEL;
        }
    } else if (dest[1] != MP_OBJ_NULL) {
        // Store.
        if (attr == MP_QSTR_verify_mode) {
            int v = mp_obj_get_int(dest[1]);
            if (v != CERT_NONE && v != CERT_REQUIRED) {
                mp_raise_ValueError(MP_ERROR_TEXT("verify_mode must be CERT_NONE or CERT_REQUIRED"));
            }
            self->verify_mode = v;
            dest[0] = MP_OBJ_NULL;  // signal: store accepted
        }
    }
}

static mp_obj_t ssl_context_load_cert_chain(mp_obj_t self_in, mp_obj_t cert, mp_obj_t pkey) {
    mp_obj_ssl_context_t *self = MP_OBJ_TO_PTR(self_in);
    self->cert = cert;
    self->key = pkey;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(ssl_context_load_cert_chain_obj, ssl_context_load_cert_chain);

// SSLContext.load_verify_locations(*, cafile=None, cadata=None)
//   - cafile: ignored (no VFS bridge to axtls); raises if non-None.
//   - cadata: bytes/str of PEM-encoded CA certs (concatenated).
// One call may load multiple certs — axtls's ssl_obj_memory_load
// walks consecutive PEM blocks when CONFIG_SSL_HAS_PEM=1.
static mp_obj_t ssl_context_load_verify_locations(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_cafile, ARG_cadata };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_cafile, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_cadata, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };

    mp_obj_ssl_context_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_cafile].u_obj != mp_const_none) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("cafile not supported (use cadata=)"));
    }
    if (args[ARG_cadata].u_obj == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("cadata is required"));
    }

    size_t len;
    const byte *data = (const byte *)mp_obj_str_get_data(args[ARG_cadata].u_obj, &len);
    int res = ssl_obj_memory_load(self->ssl_ctx, SSL_OBJ_X509_CACERT, data, (int)len, NULL);
    if (res != SSL_OK) {
        ssl_raise_error(res);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(ssl_context_load_verify_locations_obj, 1, ssl_context_load_verify_locations);

static mp_obj_t ssl_context_wrap_socket(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_server_side, ARG_do_handshake_on_connect, ARG_server_hostname };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_server_side, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_do_handshake_on_connect, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true} },
        { MP_QSTR_server_hostname, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };

    mp_obj_ssl_context_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_obj_t sock = pos_args[1];
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 2, pos_args + 2, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    return ssl_socket_make_new(self, sock, args[ARG_server_side].u_bool,
        args[ARG_do_handshake_on_connect].u_bool, args[ARG_server_hostname].u_obj);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(ssl_context_wrap_socket_obj, 2, ssl_context_wrap_socket);

static const mp_rom_map_elem_t ssl_context_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_load_cert_chain), MP_ROM_PTR(&ssl_context_load_cert_chain_obj) },
    { MP_ROM_QSTR(MP_QSTR_load_verify_locations), MP_ROM_PTR(&ssl_context_load_verify_locations_obj) },
    { MP_ROM_QSTR(MP_QSTR_wrap_socket), MP_ROM_PTR(&ssl_context_wrap_socket_obj) },
};
static MP_DEFINE_CONST_DICT(ssl_context_locals_dict, ssl_context_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    ssl_context_type,
    MP_QSTR_SSLContext,
    MP_TYPE_FLAG_NONE,
    make_new, ssl_context_make_new,
    attr, ssl_context_attr,
    locals_dict, &ssl_context_locals_dict
    );

// SSLSocket ——————————————————————————————————————————————————

static mp_obj_t ssl_socket_make_new(mp_obj_ssl_context_t *ssl_context, mp_obj_t sock,
    bool server_side, bool do_handshake_on_connect, mp_obj_t server_hostname) {

    #if MICROPY_PY_SSL_FINALISER
    mp_obj_ssl_socket_t *o = mp_obj_malloc_with_finaliser(mp_obj_ssl_socket_t, &ssl_socket_type);
    #else
    mp_obj_ssl_socket_t *o = mp_obj_malloc(mp_obj_ssl_socket_t, &ssl_socket_type);
    #endif
    o->buf = NULL;
    o->bytes_left = 0;
    o->sock = MP_OBJ_NULL;
    o->blocking = true;
    o->ssl_ctx = ssl_context->ssl_ctx;

    if (ssl_context->key != mp_const_none) {
        size_t len;
        const byte *data = (const byte *)mp_obj_str_get_data(ssl_context->key, &len);
        int res = ssl_obj_memory_load(o->ssl_ctx, SSL_OBJ_RSA_KEY, data, len, NULL);
        if (res != SSL_OK) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid key"));
        }
        data = (const byte *)mp_obj_str_get_data(ssl_context->cert, &len);
        res = ssl_obj_memory_load(o->ssl_ctx, SSL_OBJ_X509_CERT, data, len, NULL);
        if (res != SSL_OK) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid cert"));
        }
    }

    if (server_side) {
        o->ssl_sock = ssl_server_new(o->ssl_ctx, (long)sock);
    } else {
        SSL_EXTENSIONS *ext = ssl_ext_new();
        if (server_hostname != mp_const_none) {
            ext->host_name = (char *)mp_obj_str_get_str(server_hostname);
        }

        // CERT_NONE: keep SSL_SERVER_VERIFY_LATER so axtls accepts
        // any cert. CERT_REQUIRED: clear it so axtls fails the
        // handshake on chain validation errors. The CTX-level
        // options were stamped at ssl_ctx_new time; per-session
        // override goes through ssl_client_new's own option mask
        // — except axtls doesn't have one. The CTX option therefore
        // governs. When the SSLContext was created we set
        // SSL_SERVER_VERIFY_LATER unconditionally; we now (lazily)
        // adjust it on the ctx if verify_mode is CERT_REQUIRED.
        if (ssl_context->verify_mode == CERT_REQUIRED) {
            // axtls's SSL_CTX has the options on the ssl_ctx struct
            // itself; the public way to flip is to rebuild the ctx,
            // but for our config (skeleton client) we can poke it.
            // Cleaner: extend ssl_ctx_new at ctx creation time —
            // but that means we'd have to know verify_mode before
            // load_verify_locations was called. So do it here.
            o->ssl_ctx->options &= ~SSL_SERVER_VERIFY_LATER;
        } else {
            o->ssl_ctx->options |= SSL_SERVER_VERIFY_LATER;
        }
        if (!do_handshake_on_connect) {
            o->ssl_ctx->options |= SSL_CONNECT_IN_PARTS;
        } else {
            o->ssl_ctx->options &= ~SSL_CONNECT_IN_PARTS;
        }
        if (ssl_context->key != mp_const_none) {
            o->ssl_ctx->options |= SSL_NO_DEFAULT_KEY;
        }

        o->ssl_sock = ssl_client_new(o->ssl_ctx, (long)sock, NULL, 0, ext);

        if (do_handshake_on_connect) {
            int r = ssl_handshake_status(o->ssl_sock);
            if (r != SSL_OK) {
                if (r == SSL_CLOSE_NOTIFY) {
                    r = MP_ENOTCONN;
                } else if (r == SSL_EAGAIN) {
                    r = MP_EAGAIN;
                }
                ssl_raise_error(r);
            }
        }
    }

    o->sock = sock;
    return o;
}

static mp_uint_t ssl_socket_read(mp_obj_t o_in, void *buf, mp_uint_t size, int *errcode) {
    mp_obj_ssl_socket_t *o = MP_OBJ_TO_PTR(o_in);
    if (o->ssl_sock == NULL) {
        *errcode = EBADF;
        return MP_STREAM_ERROR;
    }
    while (o->bytes_left == 0) {
        mp_int_t r = ssl_read(o->ssl_sock, &o->buf);
        if (r == SSL_OK) {
            if (o->blocking) {
                continue;
            } else {
                goto eagain;
            }
        }
        if (r < 0) {
            if (r == SSL_CLOSE_NOTIFY || r == SSL_ERROR_CONN_LOST) {
                return 0;
            }
            if (r == SSL_EAGAIN) {
            eagain:
                r = MP_EAGAIN;
            }
            *errcode = r;
            return MP_STREAM_ERROR;
        }
        o->bytes_left = r;
    }
    if (size > o->bytes_left) {
        size = o->bytes_left;
    }
    memcpy(buf, o->buf, size);
    o->buf += size;
    o->bytes_left -= size;
    return size;
}

static mp_uint_t ssl_socket_write(mp_obj_t o_in, const void *buf, mp_uint_t size, int *errcode) {
    mp_obj_ssl_socket_t *o = MP_OBJ_TO_PTR(o_in);
    if (o->ssl_sock == NULL) {
        *errcode = EBADF;
        return MP_STREAM_ERROR;
    }
    mp_int_t r;
eagain:
    r = ssl_write(o->ssl_sock, buf, size);
    if (r == 0) {
        if (o->blocking) {
            goto eagain;
        } else {
            r = SSL_EAGAIN;
        }
    }
    if (r < 0) {
        if (r == SSL_CLOSE_NOTIFY || r == SSL_ERROR_CONN_LOST) {
            return 0;
        }
        if (r == SSL_EAGAIN) {
            r = MP_EAGAIN;
        }
        *errcode = r;
        return MP_STREAM_ERROR;
    }
    return r;
}

static mp_uint_t ssl_socket_ioctl(mp_obj_t o_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    mp_obj_ssl_socket_t *self = MP_OBJ_TO_PTR(o_in);
    if (request == MP_STREAM_CLOSE) {
        if (self->ssl_sock == NULL) {
            return 0;
        }
        ssl_free(self->ssl_sock);
        // Note: we DON'T ssl_ctx_free here — the ctx is owned by
        // the SSLContext now, so multiple wrap_socket calls share
        // the same ctx (and its loaded CA bundle).
        self->ssl_sock = NULL;
    }
    #if MICROPY_STREAMS_DELEGATE_ERROR
    else if (request == MP_STREAM_RAISE_ERROR) {
        ssl_raise_error((int)arg);
    }
    #endif
    if (self->sock == MP_OBJ_NULL) {
        return 0;
    }
    return mp_get_stream(self->sock)->ioctl(self->sock, request, arg, errcode);
}

static mp_obj_t ssl_socket_setblocking(mp_obj_t self_in, mp_obj_t flag_in) {
    mp_obj_ssl_socket_t *o = MP_OBJ_TO_PTR(self_in);
    mp_obj_t sock = o->sock;
    mp_obj_t dest[3];
    mp_load_method(sock, MP_QSTR_setblocking, dest);
    dest[2] = flag_in;
    mp_obj_t res = mp_call_method_n_kw(1, 0, dest);
    o->blocking = mp_obj_is_true(flag_in);
    return res;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ssl_socket_setblocking_obj, ssl_socket_setblocking);

static const mp_rom_map_elem_t ssl_socket_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_setblocking), MP_ROM_PTR(&ssl_socket_setblocking_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
    #if MICROPY_PY_SSL_FINALISER
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj) },
    #endif
};
static MP_DEFINE_CONST_DICT(ssl_socket_locals_dict, ssl_socket_locals_dict_table);

static const mp_stream_p_t ssl_socket_stream_p = {
    .read = ssl_socket_read,
    .write = ssl_socket_write,
    .ioctl = ssl_socket_ioctl,
};

static MP_DEFINE_CONST_OBJ_TYPE(
    ssl_socket_type,
    MP_QSTR_SSLSocket,
    MP_TYPE_FLAG_NONE,
    protocol, &ssl_socket_stream_p,
    locals_dict, &ssl_socket_locals_dict
    );

// `ssl` / `tls` module —————————————————————————————————————————

static const mp_rom_map_elem_t mp_module_tls_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_tls) },
    { MP_ROM_QSTR(MP_QSTR_SSLContext), MP_ROM_PTR(&ssl_context_type) },
    { MP_ROM_QSTR(MP_QSTR_PROTOCOL_TLS_CLIENT), MP_ROM_INT(PROTOCOL_TLS_CLIENT) },
    { MP_ROM_QSTR(MP_QSTR_PROTOCOL_TLS_SERVER), MP_ROM_INT(PROTOCOL_TLS_SERVER) },
    { MP_ROM_QSTR(MP_QSTR_CERT_NONE), MP_ROM_INT(CERT_NONE) },
    { MP_ROM_QSTR(MP_QSTR_CERT_REQUIRED), MP_ROM_INT(CERT_REQUIRED) },
};
static MP_DEFINE_CONST_DICT(mp_module_tls_globals, mp_module_tls_globals_table);

const mp_obj_module_t mp_module_tls = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_tls_globals,
};

#endif // MICROPY_PY_SSL && MICROPY_SSL_AXTLS
