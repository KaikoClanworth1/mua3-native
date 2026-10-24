#include "core/hle/nvn/nvn_hle.h"

#include <cstdlib>

namespace hle::nvn {
namespace {

using recomp::State;
using recomp::s32;
using recomp::s64;
using recomp::u32;
using recomp::u64;

void Return(State& s, u64 value) {
    s.x[0] = value;
    s.pc = s.x[30];
    s.icount++; // progress for the runtime (and the native-instruction statistics)
}

// NVNtextureHandle nvnDeviceGetTextureHandle(const NVNdevice*, int textureID, int samplerID): the driver packs
// both pool indices (texture in bits 0-19, sampler from bit 20) with bit 32 set.
void DeviceGetTextureHandle(State& s) {
    const u32 packed = u32(s.x[1]) | (u32(s.x[2]) << 20);
    Return(s, u64(s64(s32(packed))) | 0x100000000ull);
}

struct Entry {
    std::string_view name;
    recomp::Fn fn;
};
constexpr Entry kHostFunctions[] = {
    {"nvnDeviceGetTextureHandle", DeviceGetTextureHandle},
};

} // namespace

recomp::Fn Find(std::string_view name) {
    static const bool enabled = [] {
        const char* e = std::getenv("MUA3_NVN_HLE");
        return !(e && e[0] == '0');
    }();
    if (!enabled)
        return nullptr;
    for (const auto& e : kHostFunctions)
        if (e.name == name)
            return e.fn;
    return nullptr;
}

} // namespace hle::nvn
