// uc386-dos `os.path` submodule. Registered as the `path` attribute
// of the os module (uc386-dos/os_uc386dos.c) so user code can call
// `os.path.join(...)` / `os.path.exists(...)` etc. the way CPython
// programs expect.
//
// We don't use upstream's vfs path machinery — that ties everything
// to a VFS layer and adds noticeable surface. This shim just does
// the string manipulation on DOS-style paths (backslash separator,
// optional drive letter prefix), backed by libc's `stat()` for the
// existence checks.

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "py/runtime.h"
#include "py/objstr.h"
#include "py/objtuple.h"
#include "py/mperrno.h"

// True for backslash and forward-slash. DOS treats both as separators
// in most contexts, so we accept both for splitting / matching but
// always emit backslash when joining (matching `os.path.sep = "\\"`).
static inline int is_sep(char c) {
    return c == '\\' || c == '/';
}

// Find the rightmost separator in `s` (length `len`). Returns the
// index of the separator, or -1 if none found.
static int last_sep(const char *s, size_t len) {
    for (int i = (int)len - 1; i >= 0; i--) {
        if (is_sep(s[i])) {
            return i;
        }
    }
    return -1;
}

// Find the rightmost '.' in `s[start:len]`. Returns -1 if none, or
// if the only '.' is at the start (we treat ".foo" as having no
// extension, matching CPython).
static int last_dot(const char *s, size_t start, size_t len) {
    for (int i = (int)len - 1; i > (int)start; i--) {
        if (s[i] == '.') {
            // Skip a trailing run of dots ("foo..." has no ext) —
            // require at least one non-dot before this '.'.
            return i;
        }
        if (is_sep(s[i])) {
            return -1;
        }
    }
    return -1;
}

// `os.path.join(*parts)` — concatenate path parts with the
// platform separator. An empty `parts` returns "". A part starting
// with a drive letter or separator resets the result (matching
// CPython's behavior on Windows).
static mp_obj_t mod_path_join(size_t n_args, const mp_obj_t *args) {
    if (n_args == 0) {
        return mp_obj_new_str("", 0);
    }
    vstr_t vstr;
    vstr_init(&vstr, 64);
    for (size_t i = 0; i < n_args; i++) {
        size_t plen;
        const char *p = mp_obj_str_get_data(args[i], &plen);
        if (plen == 0) {
            continue;
        }
        // Absolute path or drive prefix → reset the buffer.
        int absolute = is_sep(p[0]);
        if (!absolute && plen >= 2 && p[1] == ':') {
            absolute = 1;
        }
        if (absolute) {
            vstr.len = 0;
        } else if (vstr.len > 0 && !is_sep(vstr.buf[vstr.len - 1])) {
            vstr_add_char(&vstr, '\\');
        }
        vstr_add_strn(&vstr, p, plen);
    }
    return mp_obj_new_str_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR(mod_path_join_obj, 0, mod_path_join);

// `os.path.split(path)` — return `(dirname, basename)`. The
// basename is the trailing component; the dirname is everything
// before it (with the trailing separator stripped, except when
// dirname would be empty or just "C:" / "/").
static mp_obj_t mod_path_split(mp_obj_t path_in) {
    size_t len;
    const char *p = mp_obj_str_get_data(path_in, &len);
    int sep = last_sep(p, len);
    mp_obj_t head, tail;
    if (sep < 0) {
        head = mp_obj_new_str("", 0);
        tail = mp_obj_new_str(p, len);
    } else {
        // Keep the separator on the head only when it's a root-level
        // marker ("\\foo" → ("\\", "foo"), "C:\\foo" → ("C:\\", "foo")).
        size_t head_len = (size_t)sep;
        if (head_len == 0 || (head_len == 2 && p[1] == ':')) {
            head_len = (size_t)sep + 1;
        }
        head = mp_obj_new_str(p, head_len);
        tail = mp_obj_new_str(p + sep + 1, len - (size_t)sep - 1);
    }
    mp_obj_t items[2] = { head, tail };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_path_split_obj, mod_path_split);

// `os.path.splitext(path)` — return `(root, ext)`. ext includes
// the leading '.' or is empty.
static mp_obj_t mod_path_splitext(mp_obj_t path_in) {
    size_t len;
    const char *p = mp_obj_str_get_data(path_in, &len);
    int sep = last_sep(p, len);
    size_t name_start = sep < 0 ? 0 : (size_t)sep + 1;
    int dot = last_dot(p, name_start, len);
    mp_obj_t root, ext;
    if (dot < 0) {
        root = mp_obj_new_str(p, len);
        ext = mp_obj_new_str("", 0);
    } else {
        root = mp_obj_new_str(p, (size_t)dot);
        ext = mp_obj_new_str(p + dot, len - (size_t)dot);
    }
    mp_obj_t items[2] = { root, ext };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_path_splitext_obj, mod_path_splitext);

// `os.path.basename(path)` — last component (just the second
// element of split()).
static mp_obj_t mod_path_basename(mp_obj_t path_in) {
    size_t len;
    const char *p = mp_obj_str_get_data(path_in, &len);
    int sep = last_sep(p, len);
    if (sep < 0) {
        return mp_obj_new_str(p, len);
    }
    return mp_obj_new_str(p + sep + 1, len - (size_t)sep - 1);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_path_basename_obj, mod_path_basename);

// `os.path.dirname(path)` — first component (just the first
// element of split()).
static mp_obj_t mod_path_dirname(mp_obj_t path_in) {
    size_t len;
    const char *p = mp_obj_str_get_data(path_in, &len);
    int sep = last_sep(p, len);
    if (sep < 0) {
        return mp_obj_new_str("", 0);
    }
    size_t head_len = (size_t)sep;
    if (head_len == 0 || (head_len == 2 && p[1] == ':')) {
        head_len = (size_t)sep + 1;
    }
    return mp_obj_new_str(p, head_len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_path_dirname_obj, mod_path_dirname);

// `os.path.exists(path)` — true if `stat()` succeeds. Backed by
// uc386's libc which routes through INT 21h AH=0x4300 (get
// attribs) and AH=0x42 (lseek to end).
static mp_obj_t mod_path_exists(mp_obj_t path_in) {
    const char *p = mp_obj_str_get_str(path_in);
    struct stat st;
    return mp_obj_new_bool(stat(p, &st) == 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_path_exists_obj, mod_path_exists);

// `os.path.isfile(path)` — same as exists for our libc, which
// only stats regular files (directories can't be opened with
// AH=0x3D). Conservative: a true return guarantees a regular
// file; a false return doesn't necessarily mean it's a dir.
static mp_obj_t mod_path_isfile(mp_obj_t path_in) {
    const char *p = mp_obj_str_get_str(path_in);
    struct stat st;
    if (stat(p, &st) != 0) {
        return mp_obj_new_bool(0);
    }
    return mp_obj_new_bool((st.st_mode & 0xF000) == 0x8000);  // S_IFREG
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_path_isfile_obj, mod_path_isfile);

// `os.path.getsize(path)` — file size in bytes, raises OSError if
// the path doesn't exist. Backed by libc stat() (INT 21h AH=0x42
// lseek-to-end via uc386's libc).
static mp_obj_t mod_path_getsize(mp_obj_t path_in) {
    const char *p = mp_obj_str_get_str(path_in);
    struct stat st;
    if (stat(p, &st) != 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    return mp_obj_new_int_from_uint((unsigned)st.st_size);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_path_getsize_obj, mod_path_getsize);

// `os.path.isabs(path)` — true if `path` starts with a separator
// or with a `<letter>:` drive prefix. Pure string check, no I/O.
static mp_obj_t mod_path_isabs(mp_obj_t path_in) {
    size_t len;
    const char *p = mp_obj_str_get_data(path_in, &len);
    if (len >= 1 && is_sep(p[0])) {
        return mp_obj_new_bool(1);
    }
    if (len >= 2 && p[1] == ':') {
        return mp_obj_new_bool(1);
    }
    return mp_obj_new_bool(0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_path_isabs_obj, mod_path_isabs);

// `os.path.abspath(path)` — normalize and prefix with getcwd() if
// the path isn't already absolute. Doesn't resolve symlinks (DOS
// doesn't have them in any meaningful sense).
static mp_obj_t mod_path_abspath(mp_obj_t path_in) {
    size_t len;
    const char *p = mp_obj_str_get_data(path_in, &len);
    int absolute = (len >= 1 && is_sep(p[0])) ||
                   (len >= 2 && p[1] == ':');
    vstr_t vstr;
    vstr_init(&vstr, len + 64);
    if (!absolute) {
        char cwd[80];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            mp_raise_OSError(MP_EIO);
        }
        vstr_add_str(&vstr, cwd);
        // getcwd typically returns "C:\\" or "C:\\subdir"; ensure
        // we have a separator before appending the relative path.
        if (vstr.len > 0 && !is_sep(vstr.buf[vstr.len - 1])) {
            vstr_add_char(&vstr, '\\');
        }
    }
    // Append `path`, deduplicating separators while we go (cheap
    // inline normpath since we already need to scan the bytes).
    int prev_sep = vstr.len > 0 && is_sep(vstr.buf[vstr.len - 1]);
    for (size_t i = 0; i < len; i++) {
        char c = p[i];
        if (is_sep(c)) {
            if (!prev_sep) {
                vstr_add_char(&vstr, '\\');
                prev_sep = 1;
            }
        } else {
            vstr_add_char(&vstr, c);
            prev_sep = 0;
        }
    }
    return mp_obj_new_str_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_path_abspath_obj, mod_path_abspath);

// `os.path.normpath(path)` — collapse forward slashes to backslash,
// drop redundant separators. Doesn't resolve `..` (DOS conventions
// vary; conservative).
static mp_obj_t mod_path_normpath(mp_obj_t path_in) {
    size_t len;
    const char *p = mp_obj_str_get_data(path_in, &len);
    vstr_t vstr;
    vstr_init(&vstr, len + 1);
    int prev_sep = 0;
    for (size_t i = 0; i < len; i++) {
        char c = p[i];
        if (is_sep(c)) {
            if (!prev_sep) {
                vstr_add_char(&vstr, '\\');
                prev_sep = 1;
            }
        } else {
            vstr_add_char(&vstr, c);
            prev_sep = 0;
        }
    }
    if (vstr.len == 0) {
        vstr_add_char(&vstr, '.');
    }
    return mp_obj_new_str_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_path_normpath_obj, mod_path_normpath);

static const mp_rom_map_elem_t mp_module_os_path_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_path) },
    { MP_ROM_QSTR(MP_QSTR_join),     MP_ROM_PTR(&mod_path_join_obj) },
    { MP_ROM_QSTR(MP_QSTR_split),    MP_ROM_PTR(&mod_path_split_obj) },
    { MP_ROM_QSTR(MP_QSTR_splitext), MP_ROM_PTR(&mod_path_splitext_obj) },
    { MP_ROM_QSTR(MP_QSTR_basename), MP_ROM_PTR(&mod_path_basename_obj) },
    { MP_ROM_QSTR(MP_QSTR_dirname),  MP_ROM_PTR(&mod_path_dirname_obj) },
    { MP_ROM_QSTR(MP_QSTR_exists),   MP_ROM_PTR(&mod_path_exists_obj) },
    { MP_ROM_QSTR(MP_QSTR_isfile),   MP_ROM_PTR(&mod_path_isfile_obj) },
    { MP_ROM_QSTR(MP_QSTR_normpath), MP_ROM_PTR(&mod_path_normpath_obj) },
    { MP_ROM_QSTR(MP_QSTR_getsize),  MP_ROM_PTR(&mod_path_getsize_obj) },
    { MP_ROM_QSTR(MP_QSTR_isabs),    MP_ROM_PTR(&mod_path_isabs_obj) },
    { MP_ROM_QSTR(MP_QSTR_abspath),  MP_ROM_PTR(&mod_path_abspath_obj) },
    { MP_ROM_QSTR(MP_QSTR_sep),      MP_ROM_QSTR(MP_QSTR__backslash_) },
};
static MP_DEFINE_CONST_DICT(mp_module_os_path_globals, mp_module_os_path_globals_table);

const mp_obj_module_t mp_module_os_path = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_os_path_globals,
};
