// uc386-dos `shutil` module — file copy/move utilities backed by
// the libc open/read/write path (which routes through INT 21h via
// uc386's libc). Lean stdlib match — `copy` / `copyfile` / `move`
// are the everyday cases; rmtree / chown / chmod aren't useful on
// DOS so we don't ship them.
//
// `move` tries os.rename first (atomic for same-volume moves under
// DOS) and falls back to copy + unlink for cross-drive moves where
// rename returns an error.

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include "py/runtime.h"
#include "py/objstr.h"
#include "py/mperrno.h"

// 4 KB buffer is a reasonable middle ground — small enough to live
// on the stack inside `_copy_stream` without bloating frame size,
// large enough that the per-call read/write syscall overhead doesn't
// dominate even on slow DOS disks.
#define SHUTIL_COPY_BUF_SIZE 4096

static void copy_stream(int src_fd, int dst_fd) {
    char buf[SHUTIL_COPY_BUF_SIZE];
    for (;;) {
        int n = read(src_fd, buf, sizeof(buf));
        if (n < 0) {
            mp_raise_OSError(MP_EIO);
        }
        if (n == 0) {
            break;
        }
        int written = 0;
        while (written < n) {
            int w = write(dst_fd, buf + written, n - written);
            if (w <= 0) {
                mp_raise_OSError(MP_EIO);
            }
            written += w;
        }
    }
}

// `shutil.copyfile(src, dst)` — overwrites dst if it exists.
// Returns dst (matching CPython).
static mp_obj_t shutil_copyfile(mp_obj_t src_in, mp_obj_t dst_in) {
    const char *src = mp_obj_str_get_str(src_in);
    const char *dst = mp_obj_str_get_str(dst_in);

    int src_fd = open(src, 0);  // O_RDONLY
    if (src_fd < 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    // Open dst write+truncate. Our libc routes a writable open
    // through INT 21h AH=0x3C (CREATE), which truncates if the
    // file exists.
    int dst_fd = open(dst, 1);  // O_WRONLY -> our libc handles create-or-truncate
    if (dst_fd < 0) {
        close(src_fd);
        mp_raise_OSError(MP_EIO);
    }
    copy_stream(src_fd, dst_fd);
    close(src_fd);
    close(dst_fd);
    return dst_in;
}
static MP_DEFINE_CONST_FUN_OBJ_2(shutil_copyfile_obj, shutil_copyfile);

// `shutil.copy(src, dst)` — alias for copyfile here. CPython
// preserves permissions through this call but DOS has none worth
// preserving, so the simpler form suffices.
static mp_obj_t shutil_copy(mp_obj_t src_in, mp_obj_t dst_in) {
    return shutil_copyfile(src_in, dst_in);
}
static MP_DEFINE_CONST_FUN_OBJ_2(shutil_copy_obj, shutil_copy);

// `shutil.move(src, dst)` — atomic rename when both ends sit on
// the same DOS volume; falls back to copy + unlink across volumes
// (which DOS rejects with INT 21h AH=0x56 → CF set, error 17).
static mp_obj_t shutil_move(mp_obj_t src_in, mp_obj_t dst_in) {
    const char *src = mp_obj_str_get_str(src_in);
    const char *dst = mp_obj_str_get_str(dst_in);

    if (rename(src, dst) == 0) {
        return dst_in;
    }
    // Cross-volume or other rename failure — fall back to
    // copy + unlink.
    shutil_copyfile(src_in, dst_in);
    if (unlink(src) != 0) {
        mp_raise_OSError(MP_EIO);
    }
    return dst_in;
}
static MP_DEFINE_CONST_FUN_OBJ_2(shutil_move_obj, shutil_move);

static const mp_rom_map_elem_t mp_module_shutil_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_shutil) },
    { MP_ROM_QSTR(MP_QSTR_copyfile), MP_ROM_PTR(&shutil_copyfile_obj) },
    { MP_ROM_QSTR(MP_QSTR_copy),     MP_ROM_PTR(&shutil_copy_obj) },
    { MP_ROM_QSTR(MP_QSTR_move),     MP_ROM_PTR(&shutil_move_obj) },
};
static MP_DEFINE_CONST_DICT(mp_module_shutil_globals, mp_module_shutil_globals_table);

const mp_obj_module_t mp_module_shutil = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_shutil_globals,
};
