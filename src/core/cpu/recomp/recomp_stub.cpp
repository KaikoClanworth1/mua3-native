// Empty function table, used when no recompiled code has been generated (see src/tools/recomp.cpp).
#include "core/cpu/recomp/recomp_rt.h"

namespace recomp {
static const Table kEmpty{nullptr, 0, nullptr, 0, nullptr, nullptr, 0};
const Table& GetTable() { return kEmpty; }
} // namespace recomp
