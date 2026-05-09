// uc386-dos custom `os` module — exposes the POSIX-style file/dir
// ops backed by uc386's libc (which translates them to INT 21h DOS
// calls). We don't use upstream's `extmod/modos.c` because that
// gates everything behind the full VFS layer (extmod/vfs.c +
// vfs_posix.c) which adds substantial code and complexity. This
// shim provides the basic surface most user code needs:
//
//   os.mkdir(path)        - INT 21h AH=0x39
//   os.rmdir(path)        - INT 21h AH=0x3A
//   os.unlink(path)       - INT 21h AH=0x41
//   os.remove(path)       - alias for unlink
//   os.rename(old, new)   - INT 21h AH=0x56
//   os.chdir(path)        - INT 21h AH=0x3B
//   os.getcwd()           - INT 21h AH=0x47 (+ drive prefix)
//
// Errors raise OSError with the DOS error code.

#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>

#include "py/runtime.h"
#include "py/mperrno.h"

#if defined(__has_include)
#  if __has_include("py/objstr.h")
#    include "py/objstr.h"
#  endif
#endif

static const char *get_path_str(mp_obj_t arg) {
    return mp_obj_str_get_str(arg);
}

static mp_obj_t mod_uc386dos_os_mkdir(mp_obj_t path_in) {
    const char *path = get_path_str(path_in);
    if (mkdir(path, 0777) != 0) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_uc386dos_os_mkdir_obj, mod_uc386dos_os_mkdir);

static mp_obj_t mod_uc386dos_os_rmdir(mp_obj_t path_in) {
    extern int rmdir(const char *path);
    const char *path = get_path_str(path_in);
    if (rmdir(path) != 0) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_uc386dos_os_rmdir_obj, mod_uc386dos_os_rmdir);

static mp_obj_t mod_uc386dos_os_unlink(mp_obj_t path_in) {
    const char *path = get_path_str(path_in);
    if (unlink(path) != 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_uc386dos_os_unlink_obj, mod_uc386dos_os_unlink);

static mp_obj_t mod_uc386dos_os_rename(mp_obj_t old_in, mp_obj_t new_in) {
    const char *old_path = get_path_str(old_in);
    const char *new_path = get_path_str(new_in);
    if (rename(old_path, new_path) != 0) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_uc386dos_os_rename_obj, mod_uc386dos_os_rename);

static mp_obj_t mod_uc386dos_os_chdir(mp_obj_t path_in) {
    const char *path = get_path_str(path_in);
    if (chdir(path) != 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_uc386dos_os_chdir_obj, mod_uc386dos_os_chdir);

static mp_obj_t mod_uc386dos_os_getcwd(void) {
    char buf[80];
    if (getcwd(buf, sizeof(buf)) == NULL) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_obj_new_str(buf, strlen(buf));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_uc386dos_os_getcwd_obj, mod_uc386dos_os_getcwd);

// `os.listdir([path])` — directory listing via DOS find-first /
// find-next. Returns a Python list of filenames (str).
//
//   - No arg: list the current directory (mask "*.*").
//   - One arg: list the given directory. We append "\\*.*" to the
//     path and pass that as the find-first mask.
//
// Skips the synthetic "." and ".." entries DOS returns for
// subdirectories (matching CPython's behavior).
// uc386 prefixes `_` to C identifiers, so C `dos_find_first` →
// asm label `_dos_find_first` (matches our libc symbol). Don't
// prepend an extra underscore in C — that would yield asm label
// `__dos_find_first`, which nasm rejects as an undefined external.
extern int dos_find_first(const char *mask);
extern int dos_find_next(void);
extern const char *dos_dta_filename(void);

// Env-block helpers — see lib/i386_dos_libc.asm for impls. They walk
// the DOS PSP environment block (PSP[0x2C] = env_seg under PMODE/W's
// flat addressing).
extern const char *getenv(const char *name);
extern const char *dos_env_iter(unsigned index);
extern int system(const char *cmd);

// `os.path` submodule. Defined in path_uc386dos.c — registered as
// the `path` attribute of mp_module_os below so user code can do
// `os.path.join(a, b)` the way CPython programs expect.
extern const struct _mp_obj_module_t mp_module_os_path;

static mp_obj_t mod_uc386dos_os_listdir(size_t n_args, const mp_obj_t *args) {
    char mask[80];
    if (n_args == 0) {
        // Current directory.
        mask[0] = '*'; mask[1] = '.'; mask[2] = '*'; mask[3] = '\0';
    } else {
        const char *path = get_path_str(args[0]);
        size_t path_len = strlen(path);
        if (path_len + 5 > sizeof(mask)) {
            mp_raise_OSError(MP_E2BIG);
        }
        memcpy(mask, path, path_len);
        // Append "\\*.*" if path doesn't already end with a separator.
        size_t i = path_len;
        if (i > 0 && mask[i - 1] != '\\' && mask[i - 1] != '/') {
            mask[i++] = '\\';
        }
        mask[i++] = '*';
        mask[i++] = '.';
        mask[i++] = '*';
        mask[i] = '\0';
    }
    mp_obj_t list = mp_obj_new_list(0, NULL);
    int rc = dos_find_first(mask);
    while (rc == 0) {
        const char *fname = dos_dta_filename();
        // Skip "." and ".." entries.
        if (!(fname[0] == '.' && (fname[1] == '\0' ||
                                  (fname[1] == '.' && fname[2] == '\0')))) {
            mp_obj_list_append(list, mp_obj_new_str(fname, strlen(fname)));
        }
        rc = dos_find_next();
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_uc386dos_os_listdir_obj,
    0, 1, mod_uc386dos_os_listdir);

// `os.stat(path)` — returns a 10-tuple matching CPython's
// `os.stat_result`:
//   (st_mode, st_ino, st_dev, st_nlink, st_uid, st_gid,
//    st_size, st_atime, st_mtime, st_ctime).
// Backed by uc386's libc `stat()` (INT 21h-based).
static mp_obj_t mod_uc386dos_os_stat(mp_obj_t path_in) {
    const char *path = get_path_str(path_in);
    struct stat st;
    if (stat(path, &st) != 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    mp_obj_t fields[10] = {
        mp_obj_new_int_from_uint(st.st_mode),
        mp_obj_new_int_from_uint(0),               // st_ino
        mp_obj_new_int_from_uint(0),               // st_dev
        mp_obj_new_int_from_uint(1),               // st_nlink
        mp_obj_new_int_from_uint(0),               // st_uid
        mp_obj_new_int_from_uint(0),               // st_gid
        mp_obj_new_int_from_uint((unsigned)st.st_size),
        mp_obj_new_int_from_uint((unsigned)st.st_atime),
        mp_obj_new_int_from_uint((unsigned)st.st_mtime),
        mp_obj_new_int_from_uint((unsigned)st.st_ctime),
    };
    return mp_obj_new_tuple(10, fields);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_uc386dos_os_stat_obj, mod_uc386dos_os_stat);

// `os.system(cmd)` — invokes the DOS shell to run `cmd`. Backed by
// libc's `system()` which calls INT 21h AH=0x4B sub 0 (LOAD AND
// EXECUTE) on COMMAND.COM (or whatever COMSPEC points at) with
// "/C <cmd>" as the command tail, then reads the exit code via
// AH=0x4D (Get Return Code). Returns -1 on exec failure.
static mp_obj_t mod_uc386dos_os_system(mp_obj_t cmd_in) {
    const char *cmd = get_path_str(cmd_in);
    int rc = system(cmd);
    return mp_obj_new_int(rc);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_uc386dos_os_system_obj, mod_uc386dos_os_system);

// `os.getenv(name, default=None)` — POSIX-style env lookup, case-
// sensitive (DOS conventionally upper-cases names but we match
// exactly what's in the env block).
static mp_obj_t mod_uc386dos_os_getenv(size_t n_args, const mp_obj_t *args) {
    const char *name = get_path_str(args[0]);
    const char *val = getenv(name);
    if (val) {
        return mp_obj_new_str(val, strlen(val));
    }
    if (n_args >= 2) {
        return args[1];
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_uc386dos_os_getenv_obj,
    1, 2, mod_uc386dos_os_getenv);

// `os.environ()` — snapshot of the env block as a fresh `dict`.
// Returns a function rather than a property because MicroPython
// modules don't support attribute-getter delegation, and the env
// block is fixed at program start so a snapshot is what we'd want
// anyway. Differs from CPython, where `os.environ` is a live dict.
static mp_obj_t mod_uc386dos_os_environ(void) {
    mp_obj_t d = mp_obj_new_dict(0);
    for (unsigned i = 0; ; i++) {
        const char *entry = dos_env_iter(i);
        if (!entry) {
            break;
        }
        const char *eq = strchr(entry, '=');
        if (!eq) {
            // Malformed entry (no '='); skip.
            continue;
        }
        size_t key_len = (size_t)(eq - entry);
        size_t val_len = strlen(eq + 1);
        mp_obj_dict_store(d,
            mp_obj_new_str(entry, key_len),
            mp_obj_new_str(eq + 1, val_len));
    }
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_uc386dos_os_environ_obj, mod_uc386dos_os_environ);

static const mp_rom_map_elem_t mp_module_os_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_os) },
    { MP_ROM_QSTR(MP_QSTR_mkdir),   MP_ROM_PTR(&mod_uc386dos_os_mkdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_rmdir),   MP_ROM_PTR(&mod_uc386dos_os_rmdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_unlink),  MP_ROM_PTR(&mod_uc386dos_os_unlink_obj) },
    { MP_ROM_QSTR(MP_QSTR_remove),  MP_ROM_PTR(&mod_uc386dos_os_unlink_obj) },
    { MP_ROM_QSTR(MP_QSTR_rename),  MP_ROM_PTR(&mod_uc386dos_os_rename_obj) },
    { MP_ROM_QSTR(MP_QSTR_chdir),   MP_ROM_PTR(&mod_uc386dos_os_chdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_getcwd),  MP_ROM_PTR(&mod_uc386dos_os_getcwd_obj) },
    { MP_ROM_QSTR(MP_QSTR_listdir), MP_ROM_PTR(&mod_uc386dos_os_listdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_stat),    MP_ROM_PTR(&mod_uc386dos_os_stat_obj) },
    { MP_ROM_QSTR(MP_QSTR_system),  MP_ROM_PTR(&mod_uc386dos_os_system_obj) },
    { MP_ROM_QSTR(MP_QSTR_getenv),  MP_ROM_PTR(&mod_uc386dos_os_getenv_obj) },
    { MP_ROM_QSTR(MP_QSTR_environ), MP_ROM_PTR(&mod_uc386dos_os_environ_obj) },
    { MP_ROM_QSTR(MP_QSTR_sep),     MP_ROM_QSTR(MP_QSTR__slash_) },
    { MP_ROM_QSTR(MP_QSTR_path),    MP_ROM_PTR(&mp_module_os_path) },
};
static MP_DEFINE_CONST_DICT(mp_module_os_globals, mp_module_os_globals_table);

const mp_obj_module_t mp_module_os = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_os_globals,
};
