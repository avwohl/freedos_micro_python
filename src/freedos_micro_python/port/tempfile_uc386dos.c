// uc386-dos `tempfile` module — minimal CPython-compat shim.
//
// Surface:
//   tempfile.gettempdir()        → str (TEMP env var, falls back to "C:\\")
//   tempfile.mktemp(suffix='', prefix='tmp', dir=None)
//                                → str path that doesn't exist yet
//
// We deliberately don't ship `mkstemp` (returns an fd) or
// `NamedTemporaryFile` (context manager) — both want to atomically
// create-and-open and DOS doesn't have O_EXCL semantics worth
// relying on. The CPython gotcha "mktemp is unsafe" applies here
// too, but the htget pattern that motivated the addition (download
// to a temp path, validate, rename) doesn't care: only one process
// is running at a time.

#include <string.h>
#include <stdio.h>

#include "py/runtime.h"
#include "py/objstr.h"
#include "py/mperrno.h"

// libc lookups — getenv comes from lib/i386_dos_libc.asm.
extern const char *getenv(const char *name);
// stat() probe to avoid handing out an existing path.
#include <sys/stat.h>

// Counter ratchets across mktemp() calls within a single REPL
// session. Combined with the BIOS-tick seed we get from PID-less
// DOS, this is enough collision avoidance for single-user
// scripting.
static unsigned tmp_counter = 0;

static mp_obj_t tempfile_gettempdir(void) {
    const char *t = getenv("TEMP");
    if (!t) {
        t = getenv("TMP");
    }
    if (!t || !*t) {
        // Fallback: root of the boot drive. Most FreeDOS systems
        // boot off C:; if that's wrong the user can SET TEMP=...
        t = "C:\\";
    }
    return mp_obj_new_str(t, strlen(t));
}
static MP_DEFINE_CONST_FUN_OBJ_0(tempfile_gettempdir_obj, tempfile_gettempdir);

// `tempfile.mktemp(suffix='', prefix='tmp', dir=None)` — return
// a string path that doesn't currently exist. Doesn't create the
// file (intentionally — caller decides open() flags).
static mp_obj_t tempfile_mktemp(size_t n_args, const mp_obj_t *pos_args,
                                mp_map_t *kw_args) {
    static const mp_arg_t allowed[] = {
        { MP_QSTR_suffix, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_)} },
        { MP_QSTR_prefix, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_tmp)} },
        { MP_QSTR_dir,    MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed),
                     allowed, args);
    const char *suffix = mp_obj_str_get_str(args[0].u_obj);
    const char *prefix = mp_obj_str_get_str(args[1].u_obj);
    const char *dir;
    if (args[2].u_obj == mp_const_none) {
        const char *t = getenv("TEMP");
        if (!t) t = getenv("TMP");
        if (!t || !*t) t = "C:\\";
        dir = t;
    } else {
        dir = mp_obj_str_get_str(args[2].u_obj);
    }

    // Loop probing names until we find one stat() rejects.
    char name[80];
    struct stat st;
    for (int attempt = 0; attempt < 1024; attempt++) {
        unsigned n = ++tmp_counter;
        // Build "<dir>\\<prefix><N><suffix>". Cap dir so the result
        // fits the DOS 8.3-ish filesystem comfortably.
        size_t dir_len = strlen(dir);
        if (dir_len > 60) dir_len = 60;
        memcpy(name, dir, dir_len);
        size_t pos = dir_len;
        if (pos > 0 && name[pos - 1] != '\\' && name[pos - 1] != '/') {
            name[pos++] = '\\';
        }
        size_t prefix_len = strlen(prefix);
        if (prefix_len > 8) prefix_len = 8;
        memcpy(name + pos, prefix, prefix_len);
        pos += prefix_len;
        // 5-digit decimal counter.
        char digits[6];
        int dlen = 0;
        unsigned v = n;
        do {
            digits[dlen++] = '0' + (v % 10);
            v /= 10;
        } while (v && dlen < 5);
        // Pad to 5.
        while (dlen < 5) digits[dlen++] = '0';
        for (int i = dlen - 1; i >= 0; i--) {
            name[pos++] = digits[i];
        }
        size_t suffix_len = strlen(suffix);
        if (pos + suffix_len + 1 > sizeof(name)) {
            mp_raise_OSError(MP_E2BIG);
        }
        memcpy(name + pos, suffix, suffix_len);
        pos += suffix_len;
        name[pos] = '\0';
        if (stat(name, &st) != 0) {
            return mp_obj_new_str(name, pos);
        }
    }
    mp_raise_OSError(MP_EEXIST);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(tempfile_mktemp_obj, 0, tempfile_mktemp);

static const mp_rom_map_elem_t mp_module_tempfile_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),    MP_ROM_QSTR(MP_QSTR_tempfile) },
    { MP_ROM_QSTR(MP_QSTR_gettempdir),  MP_ROM_PTR(&tempfile_gettempdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_mktemp),      MP_ROM_PTR(&tempfile_mktemp_obj) },
};
static MP_DEFINE_CONST_DICT(mp_module_tempfile_globals, mp_module_tempfile_globals_table);

const mp_obj_module_t mp_module_tempfile = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_tempfile_globals,
};
