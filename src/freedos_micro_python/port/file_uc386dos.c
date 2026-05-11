// uc386-dos MicroPython file I/O — port-supplied `open()` /
// `mp_import_stat()` / `mp_lexer_new_from_file()` backed by uc386's
// libc INT 21h file syscalls. Mirrors the shape of
// `extmod/vfs_posix_file.c` but without the VFS plumbing — we
// implement the file-object type directly and define
// `mp_builtin_open_obj` as the user-visible entry point.
//
// `open()` modes:
//   "r"  read-only          (default)
//   "w"  write, truncate, create
//   "a"  write, append, create
//   "+"  read-write
//   "b"  binary             (FileIO; default if no "t")
//   "t"  text               (TextIOWrapper, but we do bytes ↔ str)

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>

#include "py/builtin.h"
#include "py/lexer.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py/objstr.h"

// ---- DPMI-thunked DOS file I/O (port/dosint21_uc386dos.c) ----------
// libc's POSIX wrappers (open/read/write/close) call INT 21h directly
// from PM. PMODE/W's translation for AH=0x3F hangs from deep stacks
// (verified with the 30-line readsmoke.c standalone), which is fatal
// for MicroPython since its interpreter naturally builds a deep C
// stack before reaching user-level read() calls. We bypass that by
// dispatching INT 21h through a real-mode thunk via DPMI fn 0x0301 —
// same pattern that drives the Crynwr packet driver. The dos_int21_*
// API in dosint21_uc386dos.c handles bounce-buffer copy in/out.
extern int  dos_int21_open(const char *path, int dos_access_mode, int *err_out);
extern int  dos_int21_read(int fd, void *buf, unsigned int count, int *err_out);
extern int  dos_int21_write(int fd, const void *buf, unsigned int count, int *err_out);
extern int  dos_int21_close(int fd, int *err_out);
extern long dos_int21_lseek(int fd, long offset, int whence, int *err_out);
extern long dos_int21_fsize(int fd);

typedef struct _mp_obj_uc386dos_file_t {
    mp_obj_base_t base;
    int fd;  // -1 = closed
} mp_obj_uc386dos_file_t;

extern const mp_obj_type_t mp_type_uc386dos_textio;
extern const mp_obj_type_t mp_type_uc386dos_fileio;

// `mp_lexer_new_from_file` — read entire file into a buffer and feed
// to the str-len lexer. Used by `import xxx` to load `xxx.py`.
mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    const char *fname = qstr_str(filename);
    int err = 0;
    int fd = dos_int21_open(fname, 0 /* DOS read-only */, &err);
    if (fd < 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    long fsize = dos_int21_fsize(fd);
    if (fsize < 0) {
        dos_int21_close(fd, &err);
        mp_raise_OSError(MP_EIO);
    }
    size_t size = (size_t)fsize;
    char *buf = m_new(char, size + 1);
    size_t got = 0;
    while (got < size) {
        unsigned int want = (unsigned int)(size - got);
        int n = dos_int21_read(fd, buf + got, want, &err);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    dos_int21_close(fd, &err);
    buf[got] = '\0';
    return mp_lexer_new_from_str_len(filename, buf, got, 0);
}

// `mp_import_stat` — does `path` resolve to a file, dir, or
// nothing? `import xxx` walks `sys.path` calling this for each
// candidate path. We synthesise "does the file exist" by trying to
// open it for read; success → FILE, failure → NO_EXIST. We don't
// distinguish dirs because DOS's directory-stat path (AH=0x4E find-
// first) is a heavier dance and MicroPython only uses STAT_DIR to
// say "this is a package init" — for our flat floppy/HDD layouts
// every .py is a file.
mp_import_stat_t mp_import_stat(const char *path) {
    int err = 0;
    int fd = dos_int21_open(path, 0, &err);
    if (fd < 0) {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    dos_int21_close(fd, &err);
    return MP_IMPORT_STAT_FILE;
}

// File-object methods.
static void uc386dos_file_print(const mp_print_t *print, mp_obj_t self_in,
                                mp_print_kind_t kind) {
    (void)kind;
    mp_obj_uc386dos_file_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<io.%s %d>", mp_obj_get_type_str(self_in), self->fd);
}

static mp_uint_t uc386dos_file_read(mp_obj_t o_in, void *buf, mp_uint_t size,
                                    int *errcode) {
    mp_obj_uc386dos_file_t *o = MP_OBJ_TO_PTR(o_in);
    if (o->fd < 0) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    // Loop so a >1024-byte request can be served from the bounce buffer
    // chunk by chunk (dos_int21_read clamps each call to 1 KB to leave
    // room for the path scratch at the start of the 2 KB bounce).
    mp_uint_t total = 0;
    unsigned char *p = (unsigned char *)buf;
    while (total < size) {
        unsigned int want = (unsigned int)(size - total);
        int err = 0;
        int n = dos_int21_read(o->fd, p + total, want, &err);
        if (n < 0) {
            *errcode = MP_EIO;
            return MP_STREAM_ERROR;
        }
        if (n == 0) break;       // EOF
        total += (mp_uint_t)n;
    }
    return total;
}

static mp_uint_t uc386dos_file_write(mp_obj_t o_in, const void *buf,
                                     mp_uint_t size, int *errcode) {
    mp_obj_uc386dos_file_t *o = MP_OBJ_TO_PTR(o_in);
    if (o->fd < 0) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    mp_uint_t total = 0;
    const unsigned char *p = (const unsigned char *)buf;
    while (total < size) {
        unsigned int want = (unsigned int)(size - total);
        int err = 0;
        int n = dos_int21_write(o->fd, p + total, want, &err);
        if (n < 0) {
            *errcode = MP_EIO;
            return MP_STREAM_ERROR;
        }
        if (n == 0) break;       // disk full or similar
        total += (mp_uint_t)n;
    }
    return total;
}

static mp_uint_t uc386dos_file_ioctl(mp_obj_t o_in, mp_uint_t request,
                                     uintptr_t arg, int *errcode) {
    mp_obj_uc386dos_file_t *o = MP_OBJ_TO_PTR(o_in);
    if (request == MP_STREAM_SEEK) {
        struct mp_stream_seek_t *s = (struct mp_stream_seek_t *)(uintptr_t)arg;
        if (o->fd < 0) {
            *errcode = MP_EBADF;
            return MP_STREAM_ERROR;
        }
        // mp whence: 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END — same numeric
        // values DOS AH=0x42 uses, so no remap needed.
        int err = 0;
        long pos = dos_int21_lseek(o->fd, (long)s->offset, s->whence, &err);
        if (pos < 0) {
            *errcode = MP_EIO;
            return MP_STREAM_ERROR;
        }
        s->offset = (mp_off_t)pos;
        return 0;
    }
    if (request == MP_STREAM_FLUSH) {
        // No-op: DOS write() is unbuffered at the libc layer.
        return 0;
    }
    if (request == MP_STREAM_CLOSE) {
        if (o->fd >= 0) {
            int err = 0;
            dos_int21_close(o->fd, &err);
            o->fd = -1;
        }
        return 0;
    }
    *errcode = MP_EINVAL;
    return MP_STREAM_ERROR;
}

static const mp_rom_map_elem_t uc386dos_file_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read),     MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write),    MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),    MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_seek),     MP_ROM_PTR(&mp_stream_seek_obj) },
    { MP_ROM_QSTR(MP_QSTR_tell),     MP_ROM_PTR(&mp_stream_tell_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush),    MP_ROM_PTR(&mp_stream_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(uc386dos_file_locals_dict, uc386dos_file_locals_dict_table);

static const mp_stream_p_t uc386dos_fileio_stream_p = {
    .read = uc386dos_file_read,
    .write = uc386dos_file_write,
    .ioctl = uc386dos_file_ioctl,
};

MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_uc386dos_fileio, MP_QSTR_FileIO,
    MP_TYPE_FLAG_NONE,
    print, uc386dos_file_print,
    protocol, &uc386dos_fileio_stream_p,
    locals_dict, &uc386dos_file_locals_dict
);

static const mp_stream_p_t uc386dos_textio_stream_p = {
    .read = uc386dos_file_read,
    .write = uc386dos_file_write,
    .ioctl = uc386dos_file_ioctl,
    .is_text = true,
};

MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_uc386dos_textio, MP_QSTR_TextIOWrapper,
    MP_TYPE_FLAG_NONE,
    print, uc386dos_file_print,
    protocol, &uc386dos_textio_stream_p,
    locals_dict, &uc386dos_file_locals_dict
);

// `open(filename, mode="r")` — port-supplied entry point bound to
// the `MP_QSTR_open` builtin via `mp_builtin_open_obj` below.
static mp_obj_t uc386dos_builtin_open(size_t n_args, const mp_obj_t *args,
                                      mp_map_t *kwargs) {
    (void)kwargs;
    const char *fname = mp_obj_str_get_str(args[0]);
    const char *mode_s = "r";
    if (n_args >= 2) {
        mode_s = mp_obj_str_get_str(args[1]);
    }
    // DOS AH=0x3D access mode: 0=read, 1=write, 2=read-write. We
    // don't yet support 'w' (truncate-create) or 'a' (append-create)
    // through the thunk; those require AH=0x3C (create) or AH=0x6C
    // (extended open w/ truncate). Plumb in a later cycle. For now
    // map them to the closest read-write mode and rely on the open
    // failing if the file doesn't already exist.
    int dos_mode = 0;
    int want_create = 0;
    int want_trunc  = 0;
    const mp_obj_type_t *type = &mp_type_uc386dos_textio;
    while (*mode_s) {
        switch (*mode_s++) {
            case 'r': dos_mode = 0; break;
            case 'w': dos_mode = 1; want_create = 1; want_trunc = 1; break;
            case 'a': dos_mode = 1; want_create = 1; break;
            case '+': dos_mode = 2; break;
            case 'b': type = &mp_type_uc386dos_fileio; break;
            case 't': type = &mp_type_uc386dos_textio; break;
        }
    }
    (void)want_create; (void)want_trunc;     // TODO: AH=0x3C for create
    int err = 0;
    int fd = dos_int21_open(fname, dos_mode, &err);
    if (fd < 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    mp_obj_uc386dos_file_t *f = mp_obj_malloc(mp_obj_uc386dos_file_t, type);
    f->fd = fd;
    return MP_OBJ_FROM_PTR(f);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, uc386dos_builtin_open);
