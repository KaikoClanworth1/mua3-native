// Empty function table, used when no recompiled code has been generated (see src/tools/recomp.cpp).
#include "core/cpu/recomp/recomp_rt.h"

namespace recomp {
static u8 g_ok_stub[1];
static const Table kEmpty{nullptr, 0, nullptr, 0, g_ok_stub, nullptr, 0};
const Table& GetTable() { return kEmpty; }
} // namespace recomp
