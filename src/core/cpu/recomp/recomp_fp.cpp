// Floating-point operations of recompiled code that need exact ARM semantics: delegated to dynarmic's soft-float
// library with the guest FPCR (cumulative exception bits go to the guest FPSR as they would under the JIT).
#include <dynarmic/common/fp/fpcr.h>
#include <dynarmic/common/fp/fpsr.h>
#include <dynarmic/common/fp/op/FPConvert.h>
#include <dynarmic/common/fp/op/FPRSqrtEstimate.h>
#include <dynarmic/common/fp/op/FPRSqrtStepFused.h>
#include <dynarmic/common/fp/op/FPRecipEstimate.h>
#include <dynarmic/common/fp/op/FPRecipExponent.h>
#include <dynarmic/common/fp/op/FPRecipStepFused.h>
#include <dynarmic/common/fp/op/FPRoundInt.h>
#include <dynarmic/common/fp/op/FPToFixed.h>
#include <dynarmic/common/fp/rounding_mode.h>

#include "core/cpu/recomp/recomp_rt.h"

namespace recomp {
namespace {
namespace FP = Dynarmic::FP;

template <typename F>
auto WithFp(State& s, F&& f) {
    FP::FPSR fpsr{s.fpsr};
    auto r = f(FP::FPCR{s.fpcr}, fpsr);
    s.fpsr = fpsr.Value();
    return r;
}
FP::RoundingMode RM(u8 r) { return static_cast<FP::RoundingMode>(r); }
} // namespace

u32 FPRoundInt32(State& s, u32 a, u8 r, bool exact) {
    return u32(WithFp(s, [&](auto c, auto& f) { return FP::FPRoundInt<u32>(a, c, RM(r), exact, f); }));
}
u64 FPRoundInt64(State& s, u64 a, u8 r, bool exact) {
    return WithFp(s, [&](auto c, auto& f) { return FP::FPRoundInt<u64>(a, c, RM(r), exact, f); });
}
u32 FPRecipEstimate32(State& s, u32 a) {
    return WithFp(s, [&](auto c, auto& f) { return FP::FPRecipEstimate<u32>(a, c, f); });
}
u64 FPRecipEstimate64(State& s, u64 a) {
    return WithFp(s, [&](auto c, auto& f) { return FP::FPRecipEstimate<u64>(a, c, f); });
}
u32 FPRSqrtEstimate32(State& s, u32 a) {
    return WithFp(s, [&](auto c, auto& f) { return FP::FPRSqrtEstimate<u32>(a, c, f); });
}
u64 FPRSqrtEstimate64(State& s, u64 a) {
    return WithFp(s, [&](auto c, auto& f) { return FP::FPRSqrtEstimate<u64>(a, c, f); });
}
u32 FPRecipStepFused32(State& s, u32 a, u32 b) {
    return WithFp(s, [&](auto c, auto& f) { return FP::FPRecipStepFused<u32>(a, b, c, f); });
}
u64 FPRecipStepFused64(State& s, u64 a, u64 b) {
    return WithFp(s, [&](auto c, auto& f) { return FP::FPRecipStepFused<u64>(a, b, c, f); });
}
u32 FPRSqrtStepFused32(State& s, u32 a, u32 b) {
    return WithFp(s, [&](auto c, auto& f) { return FP::FPRSqrtStepFused<u32>(a, b, c, f); });
}
u64 FPRSqrtStepFused64(State& s, u64 a, u64 b) {
    return WithFp(s, [&](auto c, auto& f) { return FP::FPRSqrtStepFused<u64>(a, b, c, f); });
}
u32 FPRecipExponent32(State& s, u32 a) {
    return WithFp(s, [&](auto c, auto& f) { return FP::FPRecipExponent<u32>(a, c, f); });
}
u64 FPRecipExponent64(State& s, u64 a) {
    return WithFp(s, [&](auto c, auto& f) { return FP::FPRecipExponent<u64>(a, c, f); });
}
u32 FPHalfToSingle(State& s, u16 a, u8 r) {
    return WithFp(s, [&](auto c, auto& f) { return FP::FPConvert<u32, u16>(a, c, RM(r), f); });
}
u64 FPHalfToDouble(State& s, u16 a, u8 r) {
    return WithFp(s, [&](auto c, auto& f) { return FP::FPConvert<u64, u16>(a, c, RM(r), f); });
}
u16 FPSingleToHalf(State& s, u32 a, u8 r) {
    return WithFp(s, [&](auto c, auto& f) { return FP::FPConvert<u16, u32>(a, c, RM(r), f); });
}
u16 FPDoubleToHalf(State& s, u64 a, u8 r) {
    return WithFp(s, [&](auto c, auto& f) { return FP::FPConvert<u16, u64>(a, c, RM(r), f); });
}
u64 FPToFixed32(State& s, u32 a, int ibits, u8 fbits, bool uns, u8 r) {
    return WithFp(s, [&](auto c, auto& f) { return FP::FPToFixed<u32>(ibits, a, fbits, uns, c, RM(r), f); });
}
u64 FPToFixed64(State& s, u64 a, int ibits, u8 fbits, bool uns, u8 r) {
    return WithFp(s, [&](auto c, auto& f) { return FP::FPToFixed<u64>(ibits, a, fbits, uns, c, RM(r), f); });
}

} // namespace recomp
