// uc386-dos `urllib` module — package shim that exposes the `parse`
// submodule attribute. CPython's stdlib has urllib as a package with
// `parse`, `request`, `error`, `robotparser` submodules; we ship
// only `parse` (the rest involve sockets we don't have).
//
// Two import patterns this enables:
//   from urllib import parse           # works
//   import urllib; urllib.parse.quote(...)
//
// `import urllib.parse` and `from urllib.parse import quote` would
// need MP's import system to look up "urllib.parse" as a dotted path
// which it does — but only if `urllib_parse` is also a registered
// top-level module (build.sh's moduledefs.h registers both).

#include "py/runtime.h"

extern const mp_obj_module_t mp_module_urllib_parse;

static const mp_rom_map_elem_t mp_module_urllib_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_urllib) },
    { MP_ROM_QSTR(MP_QSTR_parse),    MP_ROM_PTR(&mp_module_urllib_parse) },
};
static MP_DEFINE_CONST_DICT(mp_module_urllib_globals,
                            mp_module_urllib_globals_table);

const mp_obj_module_t mp_module_urllib = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_urllib_globals,
};
