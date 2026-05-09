// uc386-dos `urllib.parse` module — port-supplied stdlib shim.
//
// Surface (CPython-compat subset, minus the rarely-used bits):
//   quote(s, safe='/')            → percent-encoded str
//   unquote(s)                    → percent-decoded str
//   quote_plus(s, safe='')        → quote + ' ' → '+'
//   unquote_plus(s)               → unquote + '+' → ' '
//   urlsplit(url)                 → SplitResult (attrtuple of 5)
//   urlencode(seq)                → form-encoded query string
//   parse_qsl(qs, keep_blank=False) → list of (key, value) tuples
//
// Skipped: urlparse / urlunparse / urlunsplit / urljoin / parse_qs
// (dict-of-lists). Add when somebody asks — current shape covers
// the parse-and-build patterns most htget-style scripts need.
//
// `urllib.parse` is the CPython idiom; we register it as a single
// flat `urllib_parse` module too (no submodule machinery required)
// so `import urllib_parse` and `from urllib import parse` both work
// once the moduledefs entry is in place.

#include <string.h>

#include "py/runtime.h"
#include "py/objstr.h"
#include "py/objtuple.h"

// RFC 3986 §2.3 unreserved characters: A-Z a-z 0-9 - _ . ~
static int is_unreserved(int c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

static int hex_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void vstr_add_pct(vstr_t *vstr, int byte) {
    static const char hex[] = "0123456789ABCDEF";
    vstr_add_byte(vstr, '%');
    vstr_add_byte(vstr, hex[(byte >> 4) & 0x0F]);
    vstr_add_byte(vstr, hex[byte & 0x0F]);
}

// Internal: encode `string` with optional `safe` char set, optional
// plus-encoding for quote_plus(). `safe_in` may be `mp_const_none`
// (use defaults). Doesn't touch mp_arg_parse_all so it's safe to
// call from internal helpers that don't have a kw_args map.
static mp_obj_t quote_inner(mp_obj_t string_in, mp_obj_t safe_in,
                            int plus) {
    mp_buffer_info_t src_buf;
    mp_get_buffer_raise(string_in, &src_buf, MP_BUFFER_READ);
    const unsigned char *src = (const unsigned char *)src_buf.buf;
    size_t n = src_buf.len;

    // For plain quote(), `safe` defaults to '/' (CPython behavior).
    // For quote_plus(), `safe` defaults to '' and ' ' becomes '+'.
    const char *safe = NULL;
    size_t safe_len = 0;
    if (safe_in != mp_const_none && mp_obj_is_str_or_bytes(safe_in)) {
        size_t sl;
        const char *s = mp_obj_str_get_data(safe_in, &sl);
        safe = s;
        safe_len = sl;
    }
    int default_safe_slash = !plus && (safe_len == 0);

    vstr_t vstr;
    vstr_init(&vstr, n);
    for (size_t i = 0; i < n; i++) {
        int c = src[i];
        int passthrough = is_unreserved(c);
        if (!passthrough) {
            if (default_safe_slash && c == '/') {
                passthrough = 1;
            }
            for (size_t j = 0; j < safe_len; j++) {
                if (c == (unsigned char)safe[j]) {
                    passthrough = 1;
                    break;
                }
            }
        }
        if (plus && c == ' ') {
            vstr_add_byte(&vstr, '+');
        } else if (passthrough) {
            vstr_add_byte(&vstr, c);
        } else {
            vstr_add_pct(&vstr, c);
        }
    }
    return mp_obj_new_str_from_vstr(&vstr);
}

// Public quote() / quote_plus() — accept (string [, safe]). Use
// 1-or-2 positional dispatch instead of mp_arg_parse_all so we
// don't need a non-NULL kw_args map; quote_inner does the actual
// work and is reusable from urlencode_pair below.
static mp_obj_t urllib_quote(size_t n_args, const mp_obj_t *args) {
    mp_obj_t safe = (n_args >= 2) ? args[1] : mp_const_none;
    return quote_inner(args[0], safe, 0);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(urllib_quote_obj, 1, 2, urllib_quote);

static mp_obj_t urllib_quote_plus(size_t n_args, const mp_obj_t *args) {
    mp_obj_t safe = (n_args >= 2) ? args[1] : mp_const_none;
    return quote_inner(args[0], safe, 1);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(urllib_quote_plus_obj, 1, 2,
                                           urllib_quote_plus);

static mp_obj_t do_unquote(mp_obj_t s_in, int plus) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(s_in, &buf, MP_BUFFER_READ);
    const unsigned char *src = (const unsigned char *)buf.buf;
    size_t n = buf.len;

    vstr_t vstr;
    vstr_init(&vstr, n);
    for (size_t i = 0; i < n; i++) {
        int c = src[i];
        if (plus && c == '+') {
            vstr_add_byte(&vstr, ' ');
        } else if (c == '%' && i + 2 < n) {
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                vstr_add_byte(&vstr, (hi << 4) | lo);
                i += 2;
            } else {
                vstr_add_byte(&vstr, c);  // malformed escape — pass through
            }
        } else {
            vstr_add_byte(&vstr, c);
        }
    }
    return mp_obj_new_str_from_vstr(&vstr);
}

static mp_obj_t urllib_unquote(mp_obj_t s_in) {
    return do_unquote(s_in, 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(urllib_unquote_obj, urllib_unquote);

static mp_obj_t urllib_unquote_plus(mp_obj_t s_in) {
    return do_unquote(s_in, 1);
}
static MP_DEFINE_CONST_FUN_OBJ_1(urllib_unquote_plus_obj, urllib_unquote_plus);

// `urlsplit(url)` → SplitResult(scheme, netloc, path, query, fragment).
// Field positions match CPython's so unpacking (s, n, p, q, f) = ...
// works the same. Doesn't separate path-params (leading `;` segment);
// users who want urlparse-style 6-tuples can split path themselves.
static mp_obj_t urllib_urlsplit(mp_obj_t url_in) {
    size_t n;
    const char *url = mp_obj_str_get_data(url_in, &n);

    size_t scheme_end = 0;
    int has_scheme = 0;
    if (n > 0 && ((url[0] >= 'A' && url[0] <= 'Z') ||
                  (url[0] >= 'a' && url[0] <= 'z'))) {
        for (size_t i = 1; i < n; i++) {
            char c = url[i];
            if (c == ':') {
                scheme_end = i;
                has_scheme = 1;
                break;
            }
            int valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '+' || c == '-' || c == '.';
            if (!valid) {
                break;
            }
        }
    }

    size_t pos = has_scheme ? scheme_end + 1 : 0;
    size_t netloc_start = pos;
    size_t netloc_end = pos;
    if (pos + 1 < n && url[pos] == '/' && url[pos + 1] == '/') {
        pos += 2;
        netloc_start = pos;
        while (pos < n && url[pos] != '/' && url[pos] != '?' &&
               url[pos] != '#') {
            pos++;
        }
        netloc_end = pos;
    }

    size_t path_start = pos;
    while (pos < n && url[pos] != '?' && url[pos] != '#') {
        pos++;
    }
    size_t path_end = pos;

    size_t query_start = path_end;
    size_t query_end = path_end;
    if (pos < n && url[pos] == '?') {
        pos++;
        query_start = pos;
        while (pos < n && url[pos] != '#') {
            pos++;
        }
        query_end = pos;
    }

    size_t frag_start = query_end;
    if (pos < n && url[pos] == '#') {
        pos++;
        frag_start = pos;
    }
    size_t frag_end = n;

    mp_obj_t items[5] = {
        mp_obj_new_str(url, has_scheme ? scheme_end : 0),
        mp_obj_new_str(url + netloc_start, netloc_end - netloc_start),
        mp_obj_new_str(url + path_start, path_end - path_start),
        mp_obj_new_str(url + query_start, query_end - query_start),
        mp_obj_new_str(url + frag_start, frag_end - frag_start),
    };
    static const qstr fields[5] = {
        MP_QSTR_scheme, MP_QSTR_netloc, MP_QSTR_path,
        MP_QSTR_query,  MP_QSTR_fragment,
    };
    return mp_obj_new_attrtuple(fields, 5, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(urllib_urlsplit_obj, urllib_urlsplit);

// `urlencode(query)` — accepts a dict OR an iterable of (k, v)
// pairs. Each value is run through quote_plus.
static void urlencode_pair(vstr_t *vstr, mp_obj_t key, mp_obj_t val,
                           int *first) {
    if (!*first) {
        vstr_add_byte(vstr, '&');
    }
    *first = 0;
    // Quote key (always a str/bytes per dict-key contract).
    mp_obj_t k_quoted = quote_inner(key, mp_const_none, 1);
    size_t klen;
    const char *ks = mp_obj_str_get_data(k_quoted, &klen);
    vstr_add_strn(vstr, ks, klen);
    vstr_add_byte(vstr, '=');
    // Quote value. Coerce ints/floats/etc. via str(val).
    if (!mp_obj_is_str_or_bytes(val)) {
        val = mp_call_function_1(MP_OBJ_FROM_PTR(&mp_type_str), val);
    }
    mp_obj_t v_quoted = quote_inner(val, mp_const_none, 1);
    size_t vlen;
    const char *vs = mp_obj_str_get_data(v_quoted, &vlen);
    vstr_add_strn(vstr, vs, vlen);
}

static mp_obj_t urllib_urlencode(mp_obj_t query_in) {
    vstr_t vstr;
    vstr_init(&vstr, 64);
    int first = 1;
    if (mp_obj_is_type(query_in, &mp_type_dict)) {
        // Walk the dict's internal map directly. Public APIs would
        // require either a key-only iterator + per-key lookup, or
        // mp_map_lookup with MP_MAP_LOOKUP_KIND_REMOVE_IF_FOUND
        // games. The struct fields (`map`, `map.alloc`, `map.table`,
        // `MP_MAP_SLOT_IS_FILLED`) are stable public surface — used
        // throughout extmod/.
        mp_obj_dict_t *d = MP_OBJ_TO_PTR(query_in);
        for (size_t i = 0; i < d->map.alloc; i++) {
            if (MP_MAP_SLOT_IS_FILLED(&d->map, i)) {
                urlencode_pair(&vstr, d->map.table[i].key,
                               d->map.table[i].value, &first);
            }
        }
    } else {
        mp_obj_iter_buf_t iter_buf;
        mp_obj_t iter = mp_getiter(query_in, &iter_buf);
        mp_obj_t pair;
        while ((pair = mp_iternext(iter)) != MP_OBJ_STOP_ITERATION) {
            mp_obj_t *items;
            size_t pn;
            mp_obj_get_array(pair, &pn, &items);
            if (pn != 2) {
                mp_raise_ValueError(MP_ERROR_TEXT(
                    "urlencode pair not a 2-tuple"));
            }
            urlencode_pair(&vstr, items[0], items[1], &first);
        }
    }
    return mp_obj_new_str_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_1(urllib_urlencode_obj, urllib_urlencode);

// `parse_qsl(qs, keep_blank_values=False)` — split on `&`, then `=`,
// applying unquote_plus to each side. Returns a list of (k, v).
static mp_obj_t urllib_parse_qsl(size_t n_args, const mp_obj_t *pos_args,
                                 mp_map_t *kw_args) {
    static const mp_arg_t allowed[] = {
        { MP_QSTR_qs, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_keep_blank_values, MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed),
                     allowed, args);

    size_t n;
    const char *qs = mp_obj_str_get_data(args[0].u_obj, &n);
    int keep_blank = args[1].u_bool;

    mp_obj_t result = mp_obj_new_list(0, NULL);
    size_t i = 0;
    while (i < n) {
        // Find next `&` or end.
        size_t pair_end = i;
        while (pair_end < n && qs[pair_end] != '&' && qs[pair_end] != ';') {
            pair_end++;
        }
        // Within [i, pair_end), find `=`.
        size_t eq = i;
        while (eq < pair_end && qs[eq] != '=') {
            eq++;
        }
        size_t key_len = eq - i;
        size_t val_off = (eq < pair_end) ? eq + 1 : pair_end;
        size_t val_len = pair_end - val_off;
        // CPython parse_qsl drops empty-value pairs unless
        // keep_blank_values. Empty *key* always drops.
        if (key_len > 0 && (val_len > 0 || keep_blank)) {
            mp_obj_t key_raw = mp_obj_new_str(qs + i, key_len);
            mp_obj_t val_raw = mp_obj_new_str(qs + val_off, val_len);
            mp_obj_t key = do_unquote(key_raw, 1);
            mp_obj_t val = do_unquote(val_raw, 1);
            mp_obj_t pair_items[2] = { key, val };
            mp_obj_list_append(result,
                               mp_obj_new_tuple(2, pair_items));
        }
        i = pair_end + 1;
    }
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(urllib_parse_qsl_obj, 1, urllib_parse_qsl);

static const mp_rom_map_elem_t mp_module_urllib_parse_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),     MP_ROM_QSTR(MP_QSTR_urllib_parse) },
    { MP_ROM_QSTR(MP_QSTR_quote),        MP_ROM_PTR(&urllib_quote_obj) },
    { MP_ROM_QSTR(MP_QSTR_unquote),      MP_ROM_PTR(&urllib_unquote_obj) },
    { MP_ROM_QSTR(MP_QSTR_quote_plus),   MP_ROM_PTR(&urllib_quote_plus_obj) },
    { MP_ROM_QSTR(MP_QSTR_unquote_plus), MP_ROM_PTR(&urllib_unquote_plus_obj) },
    { MP_ROM_QSTR(MP_QSTR_urlsplit),     MP_ROM_PTR(&urllib_urlsplit_obj) },
    { MP_ROM_QSTR(MP_QSTR_urlencode),    MP_ROM_PTR(&urllib_urlencode_obj) },
    { MP_ROM_QSTR(MP_QSTR_parse_qsl),    MP_ROM_PTR(&urllib_parse_qsl_obj) },
};
static MP_DEFINE_CONST_DICT(mp_module_urllib_parse_globals,
                            mp_module_urllib_parse_globals_table);

const mp_obj_module_t mp_module_urllib_parse = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_urllib_parse_globals,
};
