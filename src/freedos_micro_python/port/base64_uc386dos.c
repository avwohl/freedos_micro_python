// uc386-dos `base64` module — thin port-supplied stdlib shim.
//
// MicroPython ships base64 routines in `binascii` (b2a_base64 /
// a2b_base64) but most CPython programs reach for `import base64;
// base64.b64encode(...)` directly. Rather than freezing a Python
// wrapper, we ship a tiny C module with inline RFC 4648 base64 +
// base16 encoders/decoders. ~80 lines, no allocations beyond the
// vstr that holds the result.
//
// Surface:
//   base64.b64encode(data)         → bytes (no trailing newline)
//   base64.b64decode(s)            → bytes
//   base64.b16encode(data)         → uppercase hex bytes
//   base64.b16decode(s)            → bytes (case-insensitive accepted)

#include <string.h>

#include "py/runtime.h"
#include "py/objstr.h"
#include "py/binary.h"

static const char b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_decode_char(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;  // invalid (also catches '=' padding)
}

static mp_obj_t base64_b64encode(mp_obj_t data_in) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data_in, &buf, MP_BUFFER_READ);
    const unsigned char *src = (const unsigned char *)buf.buf;
    size_t n = buf.len;

    vstr_t vstr;
    vstr_init_len(&vstr, ((n + 2) / 3) * 4);
    char *out = vstr.buf;

    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        unsigned int v = ((unsigned int)src[i] << 16) |
                         ((unsigned int)src[i + 1] << 8) |
                         (unsigned int)src[i + 2];
        *out++ = b64_alphabet[(v >> 18) & 0x3F];
        *out++ = b64_alphabet[(v >> 12) & 0x3F];
        *out++ = b64_alphabet[(v >> 6) & 0x3F];
        *out++ = b64_alphabet[v & 0x3F];
    }
    size_t rem = n - i;
    if (rem == 1) {
        unsigned int v = (unsigned int)src[i] << 16;
        *out++ = b64_alphabet[(v >> 18) & 0x3F];
        *out++ = b64_alphabet[(v >> 12) & 0x3F];
        *out++ = '=';
        *out++ = '=';
    } else if (rem == 2) {
        unsigned int v = ((unsigned int)src[i] << 16) |
                         ((unsigned int)src[i + 1] << 8);
        *out++ = b64_alphabet[(v >> 18) & 0x3F];
        *out++ = b64_alphabet[(v >> 12) & 0x3F];
        *out++ = b64_alphabet[(v >> 6) & 0x3F];
        *out++ = '=';
    }
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_1(base64_b64encode_obj, base64_b64encode);

static mp_obj_t base64_b64decode(mp_obj_t s_in) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(s_in, &buf, MP_BUFFER_READ);
    const unsigned char *src = (const unsigned char *)buf.buf;
    size_t n = buf.len;

    vstr_t vstr;
    vstr_init(&vstr, (n * 3) / 4 + 4);
    int bits = 0;
    int collected = 0;
    for (size_t i = 0; i < n; i++) {
        int c = src[i];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            continue;
        }
        if (c == '=') {
            break;  // padding — no more data
        }
        int v = b64_decode_char(c);
        if (v < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid base64 character"));
        }
        bits = (bits << 6) | v;
        collected += 6;
        if (collected >= 8) {
            collected -= 8;
            vstr_add_byte(&vstr, (bits >> collected) & 0xFF);
        }
    }
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_1(base64_b64decode_obj, base64_b64decode);

static mp_obj_t base64_b16encode(mp_obj_t data_in) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data_in, &buf, MP_BUFFER_READ);
    const unsigned char *src = (const unsigned char *)buf.buf;
    static const char hex[] = "0123456789ABCDEF";

    vstr_t vstr;
    vstr_init_len(&vstr, buf.len * 2);
    char *out = vstr.buf;
    for (size_t i = 0; i < buf.len; i++) {
        *out++ = hex[(src[i] >> 4) & 0x0F];
        *out++ = hex[src[i] & 0x0F];
    }
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_1(base64_b16encode_obj, base64_b16encode);

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static mp_obj_t base64_b16decode(mp_obj_t s_in) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(s_in, &buf, MP_BUFFER_READ);
    const unsigned char *src = (const unsigned char *)buf.buf;
    if (buf.len & 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("odd-length base16 input"));
    }
    vstr_t vstr;
    vstr_init_len(&vstr, buf.len / 2);
    char *out = vstr.buf;
    for (size_t i = 0; i < buf.len; i += 2) {
        int hi = hex_digit(src[i]);
        int lo = hex_digit(src[i + 1]);
        if (hi < 0 || lo < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid base16 character"));
        }
        *out++ = (hi << 4) | lo;
    }
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_1(base64_b16decode_obj, base64_b16decode);

static const mp_rom_map_elem_t mp_module_base64_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),  MP_ROM_QSTR(MP_QSTR_base64) },
    { MP_ROM_QSTR(MP_QSTR_b64encode), MP_ROM_PTR(&base64_b64encode_obj) },
    { MP_ROM_QSTR(MP_QSTR_b64decode), MP_ROM_PTR(&base64_b64decode_obj) },
    { MP_ROM_QSTR(MP_QSTR_b16encode), MP_ROM_PTR(&base64_b16encode_obj) },
    { MP_ROM_QSTR(MP_QSTR_b16decode), MP_ROM_PTR(&base64_b16decode_obj) },
};
static MP_DEFINE_CONST_DICT(mp_module_base64_globals, mp_module_base64_globals_table);

const mp_obj_module_t mp_module_base64 = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_base64_globals,
};
