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
    int fd = open(fname, O_RDONLY);
    if (fd < 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    // Stat to get the file size. Avoids a grow-the-buffer loop.
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        mp_raise_OSError(MP_EIO);
    }
    size_t size = (size_t)st.st_size;
    char *buf = m_new(char, size + 1);
    size_t got = 0;
    while (got < size) {
        int n = read(fd, buf + got, size - got);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    close(fd);
    buf[got] = '\0';
    return mp_lexer_new_from_str_len(filename, buf, got, 0);
}

// `mp_import_stat` — does `path` resolve to a file, dir, or
// nothing? `import xxx` walks `sys.path` calling this for each
// candidate path.
mp_import_stat_t mp_import_stat(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    if (S_ISDIR(st.st_mode)) {
        return MP_IMPORT_STAT_DIR;
    }
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
    int r = read(o->fd, buf, size);
    if (r < 0) {
        *errcode = MP_EIO;
        return MP_STREAM_ERROR;
    }
    return (mp_uint_t)r;
}

static mp_uint_t uc386dos_file_write(mp_obj_t o_in, const void *buf,
                                     mp_uint_t size, int *errcode) {
    mp_obj_uc386dos_file_t *o = MP_OBJ_TO_PTR(o_in);
    if (o->fd < 0) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    int r = write(o->fd, buf, size);
    if (r < 0) {
        *errcode = MP_EIO;
        return MP_STREAM_ERROR;
    }
    return (mp_uint_t)r;
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
        int whence = (s->whence == 0) ? SEEK_SET
                   : (s->whence == 1) ? SEEK_CUR
                                      : SEEK_END;
        long pos = lseek(o->fd, (long)s->offset, whence);
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
            close(o->fd);
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
    int mode_rw = O_RDONLY;
    int mode_x = 0;
    const mp_obj_type_t *type = &mp_type_uc386dos_textio;
    while (*mode_s) {
        switch (*mode_s++) {
            case 'r': mode_rw = O_RDONLY; break;
            case 'w': mode_rw = O_WRONLY; mode_x = O_CREAT | O_TRUNC; break;
            case 'a': mode_rw = O_WRONLY; mode_x = O_CREAT | O_APPEND; break;
            case '+': mode_rw = O_RDWR; break;
            case 'b': type = &mp_type_uc386dos_fileio; break;
            case 't': type = &mp_type_uc386dos_textio; break;
        }
    }
    int fd = open(fname, mode_rw | mode_x, 0644);
    if (fd < 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    mp_obj_uc386dos_file_t *f = mp_obj_malloc(mp_obj_uc386dos_file_t, type);
    f->fd = fd;
    return MP_OBJ_FROM_PTR(f);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, uc386dos_builtin_open);
