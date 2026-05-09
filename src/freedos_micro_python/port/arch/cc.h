// Stub that forwards to the real arch/cc.h shim (lwip-arch-cc.h).
// lwIP's headers do `#include "arch/cc.h"`; we put that path on
// the search list (uc386-dos/) so this file resolves there.
#include "../lwip-arch-cc.h"
