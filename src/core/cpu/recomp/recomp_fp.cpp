// Floating-point operations of recompiled code that need exact ARM semantics, in native code (no soft-float
// library): integer bit manipulation where the ARM result is defined bit by bit (estimates, conversions to
// fixed point and half precision), the host FPU where it rounds the same way (MXCSR mirrors FPCR rounding and
// flush-to-zero while native code runs, see cpu_dynarmic.cpp), AES-NI for the AES rounds.
//
// Semantics follow the ARM pseudocode (and dynarmic, which src/tools/fp_selftest.cpp compares against): FPCR
// rounding mode, FZ (flush denormal inputs, IDC), DN (default NaN), AHP for half precision, NaN quieting and
// propagation order, saturation. Cumulative FPSR flags: IOC, DZC, IDC and every flag of the integer paths are
// exact; IXC/OFC/UFC of FRECPS/FRSQRTS (host FPU) are not raised, like every other native FP op (FADD etc.). The
// game only reads FPSR through libc's fenv functions.
#include "core/cpu/recomp/recomp_rt.h"

namespace recomp {
namespace {

constexpr u32 kIOC = 1u << 0, kDZC = 1u << 1, kOFC = 1u << 2, kUFC = 1u << 3, kIXC = 1u << 4, kIDC = 1u << 7;
constexpr u32 kFpcrFZ = 1u << 24, kFpcrDN = 1u << 25, kFpcrAHP = 1u << 26;
enum Rounding : u8 { kRN = 0, kRP = 1, kRM = 2, kRZ = 3, kRA = 4 }; // dynarmic RoundingMode order

// Bit layout of a binary floating-point format.
template <typename U>
struct Fmt;
template <>
struct Fmt<u16> {
    static constexpr int kMant = 10, kExpBits = 5;
};
template <>
struct Fmt<u32> {
    static constexpr int kMant = 23, kExpBits = 8;
};
template <>
struct Fmt<u64> {
    static constexpr int kMant = 52, kExpBits = 11;
};
template <typename U>
struct Info : Fmt<U> {
    using Fmt<U>::kMant;
    using Fmt<U>::kExpBits;
    static constexpr int kBits = int(sizeof(U) * 8);
    static constexpr int kBias = (1 << (kExpBits - 1)) - 1;
    static constexpr U kExpMax = U((1u << kExpBits) - 1);
    static constexpr U kMantMask = U((U(1) << kMant) - 1);
    static constexpr U kSign = U(U(1) << (kBits - 1));
    static constexpr U kQuietBit = U(U(1) << (kMant - 1));
    static constexpr U kInf = U(kExpMax << kMant);
    static constexpr U kDefaultNaN = U(kInf | kQuietBit);
    static constexpr U kMaxNormal = U(((kExpMax - 1) << kMant) | kMantMask);
    static bool Sign(U a) { return (a >> (kBits - 1)) != 0; }
    static U Exp(U a) { return U((a >> kMant) & kExpMax); }
    static U Frac(U a) { return U(a & kMantMask); }
};

bool FZ(const State& s) { return (s.fpcr & kFpcrFZ) != 0; }
u8 FpcrRounding(const State& s) { return u8((s.fpcr >> 22) & 3); }

// FPProcessNaN: signalling NaNs are quieted (IOC); DN replaces any NaN with the default NaN.
template <typename U>
U ProcessNaN(State& s, U a) {
    using I = Info<U>;
    if (!(a & I::kQuietBit))
        s.fpsr |= kIOC;
    return (s.fpcr & kFpcrDN) ? I::kDefaultNaN : U(a | I::kQuietBit);
}
template <typename U>
bool IsNaNBits(U a) {
    using I = Info<U>;
    return I::Exp(a) == I::kExpMax && I::Frac(a) != 0;
}
template <typename U>
bool IsSNaNBits(U a) {
    return IsNaNBits(a) && !(a & Info<U>::kQuietBit);
}

// Overflowed result: infinity or the largest normal number, by rounding direction.
template <typename U>
U OverflowResult(bool sign, u8 rounding) {
    using I = Info<U>;
    const bool to_inf = rounding == kRN || rounding == kRA || (rounding == kRP && !sign) || (rounding == kRM && sign);
    return U((sign ? I::kSign : 0) | (to_inf ? I::kInf : I::kMaxNormal));
}

// FPRound for a finite nonzero value mant * 2^exp (mant != 0, any width up to 64 bits) to format U with an
// explicit rounding mode (RN/RP/RM/RZ). fz: flush tiny results to zero (before rounding, UFC). ahp: the result
// is ARM alternative half precision (no infinities or NaNs: exponent 31 is a normal binade).
template <typename U>
U RoundPack(State& s, bool sign, int exp, u64 mant, u8 rounding, bool fz, bool ahp = false) {
    using I = Info<U>;
    constexpr int kMinExp = 1 - I::kBias; // exponent of the smallest normal number
    const int top = 63 - std::countl_zero(mant);
    int e = exp + top; // the value is in [2^e, 2^(e+1))
    const U sign_bits = sign ? I::kSign : U(0);
    if (fz && e < kMinExp) {
        s.fpsr |= kUFC;
        return sign_bits;
    }
    // Keep kMant+1 significant bits (fewer for subnormal results); the rest decides the rounding.
    const int keep_shift = top - I::kMant + (e < kMinExp ? kMinExp - e : 0); // right shift of mant
    u64 m;
    bool half = false, sticky = false;
    if (keep_shift <= 0) {
        m = mant << -keep_shift;
    } else if (keep_shift >= 64) {
        m = 0;
        half = keep_shift == 64 && (mant >> 63);
        sticky = keep_shift == 64 ? (mant << 1) != 0 : true;
    } else {
        m = mant >> keep_shift;
        half = (mant >> (keep_shift - 1)) & 1;
        sticky = keep_shift > 1 && (mant & ((u64(1) << (keep_shift - 1)) - 1)) != 0;
    }
    const bool inexact = half || sticky;
    int biased = e < kMinExp ? 0 : e + I::kBias;
    if (biased == 0 && inexact)
        s.fpsr |= kUFC;
    bool up = false;
    switch (rounding) {
    case kRN: up = half && (sticky || (m & 1)); break;
    case kRP: up = inexact && !sign; break;
    case kRM: up = inexact && sign; break;
    default: break;
    }
    if (up) {
        ++m;
        if (biased == 0 && m == (u64(1) << I::kMant)) {
            biased = 1; // rounded up from subnormal to the smallest normal number
        } else if (m == (u64(1) << (I::kMant + 1))) {
            m >>= 1;
            ++biased;
        }
    }
    const int max_biased = ahp ? (1 << I::kExpBits) : int(I::kExpMax);
    if (biased >= max_biased) {
        if (ahp) {
            s.fpsr |= kIOC;
            return U(sign_bits | 0x7FFF);
        }
        s.fpsr |= kOFC | kIXC;
        return OverflowResult<U>(sign, rounding);
    }
    if (inexact)
        s.fpsr |= kIXC;
    return U(sign_bits | (U(biased) << I::kMant) | (U(m) & I::kMantMask));
}

// Finite nonzero operand as mant * 2^exp (no normalization). Denormals must have been handled by the caller.
template <typename U>
void Decompose(U a, int& exp, u64& mant) {
    using I = Info<U>;
    const U e = I::Exp(a);
    if (e == 0) {
        mant = I::Frac(a);
        exp = 1 - I::kBias - I::kMant;
    } else {
        mant = u64(I::Frac(a)) | (u64(1) << I::kMant);
        exp = int(e) - I::kBias - I::kMant;
    }
}

// ---- reciprocal and reciprocal square root estimates (FRECPE, FRSQRTE): ARM's 8-bit tables
struct EstimateTables {
    u8 recip[256];
    u8 rsqrt[512];
    constexpr EstimateTables() : recip(), rsqrt() {
        for (u32 i = 0; i < 256; ++i) {
            const u32 a = (i + 256) * 2 + 1;
            const u32 b = (1u << 19) / a;
            recip[i] = u8((b + 1) / 2);
        }
        for (u32 i = 128; i < 512; ++i) {
            const u64 a = i < 256 ? i * 2 + 1 : (i | 1) * 2;
            u64 b = 512;
            while (a * (b + 1) * (b + 1) < (u64(1) << 28))
                ++b;
            rsqrt[i] = u8((b + 1) / 2);
        }
    }
};
constexpr EstimateTables kEstimate;

template <typename U>
U RecipEstimate(State& s, U a) {
    using I = Info<U>;
    const bool sign = I::Sign(a);
    const U sign_bits = sign ? I::kSign : U(0);
    const U e = I::Exp(a), frac = I::Frac(a);
    if (e == I::kExpMax)
        return frac ? ProcessNaN(s, a) : sign_bits; // 1/inf = 0
    if (e == 0 && (frac == 0 || FZ(s))) {
        if (frac)
            s.fpsr |= kIDC;
        s.fpsr |= kDZC;
        return U(sign_bits | I::kInf);
    }
    if (e == 0 && frac < (U(1) << (I::kMant - 2))) { // |a| < 2^(emin-2): the reciprocal overflows
        s.fpsr |= kOFC | kIXC;
        return OverflowResult<U>(sign, FpcrRounding(s));
    }
    if (FZ(s) && int(e) >= 2 * I::kBias - 1) { // |a| >= 2^(bias-1): the reciprocal is subnormal
        s.fpsr |= kUFC;
        return sign_bits;
    }
    // ARM pseudocode with a 52-bit fraction
    u64 f = u64(frac) << (52 - I::kMant);
    int exp = int(e);
    if (e == 0) {
        if (!((f >> 51) & 1)) {
            exp = -1;
            f = (f << 2) & ((u64(1) << 52) - 1);
        } else {
            f = (f << 1) & ((u64(1) << 52) - 1);
        }
    }
    int rexp = 2 * I::kBias - 1 - exp;
    u64 rf = u64(kEstimate.recip[f >> 44]) << 44;
    if (rexp == 0) {
        rf = (u64(1) << 51) | (rf >> 1);
    } else if (rexp == -1) {
        rf = (u64(1) << 50) | (rf >> 2);
        rexp = 0;
    }
    return U(sign_bits | (U(rexp) << I::kMant) | U(rf >> (52 - I::kMant)));
}

template <typename U>
U RSqrtEstimate(State& s, U a) {
    using I = Info<U>;
    const bool sign = I::Sign(a);
    const U e = I::Exp(a), frac = I::Frac(a);
    if (e == I::kExpMax && frac)
        return ProcessNaN(s, a);
    if (e == 0 && (frac == 0 || FZ(s))) {
        if (frac)
            s.fpsr |= kIDC;
        s.fpsr |= kDZC;
        return U((sign ? I::kSign : U(0)) | I::kInf);
    }
    if (sign) {
        s.fpsr |= kIOC;
        return I::kDefaultNaN;
    }
    if (e == I::kExpMax)
        return 0; // 1/sqrt(+inf) = +0
    // Normalized: value = 1.m * 2^vexp, m = the 8 fraction bits after the leading one
    int vexp;
    u64 norm; // leading one at bit 62
    if (e == 0) {
        const int top = 63 - std::countl_zero(u64(frac));
        vexp = 1 - I::kBias - I::kMant + top;
        norm = u64(frac) << (62 - top);
    } else {
        vexp = int(e) - I::kBias;
        norm = (u64(frac) | (u64(1) << I::kMant)) << (62 - I::kMant);
    }
    const int rexp = (-(vexp + 1)) >> 1;
    const u64 scaled = norm >> (vexp % 2 == 0 ? 55 : 54);
    const u64 est = kEstimate.rsqrt[scaled & 0x1FF];
    return U((U(rexp + I::kBias) << I::kMant) | (U(est << (I::kMant - 8)) & I::kMantMask));
}

template <typename U>
U RecipExponent(State& s, U a) {
    using I = Info<U>;
    const U e = I::Exp(a), frac = I::Frac(a);
    if (e == I::kExpMax && frac)
        return ProcessNaN(s, a);
    if (e == 0 && frac && FZ(s))
        s.fpsr |= kIDC;
    const U sign_bits = I::Sign(a) ? I::kSign : U(0);
    if (e == 0)
        return U(sign_bits | ((I::kExpMax - 1) << I::kMant));
    return U(sign_bits | ((U(~e) & I::kExpMax) << I::kMant));
}

// ---- FRECPS / FRSQRTS: 2 - a*b and (3 - a*b) / 2, fused
// The exact result is never tiny (the product's own precision keeps 2 - a*b or 3 - a*b either zero or far above the
// smallest normal number), so the host's fused multiply-add rounds it exactly like ARM in every rounding mode,
// with or without flush-to-zero (DAZ flushes denormal inputs like FZ). FRSQRTS halves an exactly representable
// operand first, so its intermediate cannot overflow.
template <typename U>
using HostFloat = std::conditional_t<sizeof(U) == 4, float, double>;
template <typename U>
HostFloat<U> FnMAdd(HostFloat<U> a, HostFloat<U> b, HostFloat<U> c) { // c - a*b, one rounding
    if constexpr (sizeof(U) == 4)
        return _mm_cvtss_f32(_mm_fnmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c)));
    else
        return _mm_cvtsd_f64(_mm_fnmadd_sd(_mm_set_sd(a), _mm_set_sd(b), _mm_set_sd(c)));
}
template <typename U, bool kRSqrt>
U StepFused(State& s, U a, U b) {
    using I = Info<U>;
    using H = HostFloat<U>;
    const U ea = I::Exp(a), eb = I::Exp(b);
    if (ea == 0 || ea == I::kExpMax || eb == 0 || eb == I::kExpMax) [[unlikely]] {
        if (FZ(s)) { // inputs are unpacked (IDC) before the NaN checks
            if (ea == 0 && I::Frac(a)) {
                s.fpsr |= kIDC;
                a &= I::kSign;
            }
            if (eb == 0 && I::Frac(b)) {
                s.fpsr |= kIDC;
                b &= I::kSign;
            }
        }
        if (IsNaNBits(a) || IsNaNBits(b)) {
            const U na = U(a ^ I::kSign); // ARM negates the first operand before the NaN checks
            if (IsSNaNBits(na))
                return ProcessNaN(s, na);
            if (IsSNaNBits(b))
                return ProcessNaN(s, b);
            return ProcessNaN(s, IsNaNBits(na) ? na : b);
        }
        const bool zero_a = (a & ~I::kSign) == 0, zero_b = (b & ~I::kSign) == 0;
        if ((ea == I::kExpMax && zero_b) || (eb == I::kExpMax && zero_a))
            return std::bit_cast<U>(kRSqrt ? H(1.5) : H(2.0)); // inf * 0
    }
    const H fa = std::bit_cast<H>(a), fb = std::bit_cast<H>(b);
    if constexpr (!kRSqrt) {
        return std::bit_cast<U>(FnMAdd<U>(fa, fb, H(2.0)));
    } else {
        // (3 - a*b) / 2 = 1.5 - (a/2)*b: halve whichever operand halves exactly (a normal one above the smallest
        // binade). With both that small the product is tiny, 3 - a*b is close to 3 and halving it is exact.
        if (ea >= 2 && ea != I::kExpMax)
            return std::bit_cast<U>(FnMAdd<U>(fa * H(0.5), fb, H(1.5)));
        if (eb >= 2 && eb != I::kExpMax)
            return std::bit_cast<U>(FnMAdd<U>(fa, fb * H(0.5), H(1.5)));
        if (ea == I::kExpMax || eb == I::kExpMax) // infinity times a nonzero finite value
            return std::bit_cast<U>(FnMAdd<U>(fa, fb, H(3.0)));
        return std::bit_cast<U>(FnMAdd<U>(fa, fb, H(3.0)) * H(0.5));
    }
}

// ---- FRINT*: round to an integral value in the same format
template <typename U>
U RoundInt(State& s, U a, u8 rounding, bool exact) {
    using I = Info<U>;
    using H = HostFloat<U>;
    const U e = I::Exp(a), frac = I::Frac(a);
    if (e == I::kExpMax)
        return frac ? ProcessNaN(s, a) : a;
    if (e == 0 && frac && FZ(s)) {
        s.fpsr |= kIDC;
        return U(a & I::kSign);
    }
    if (int(e) >= I::kBias + I::kMant)
        return a; // already integral (and zero is zero)
    if (e == 0 && frac == 0)
        return a;
    // SSE4.1 roundss/roundsd with the mode in the instruction, exceptions suppressed; ties away from zero by hand.
    U r;
    if constexpr (sizeof(U) == 4) {
        const __m128 x = _mm_set_ss(std::bit_cast<float>(a));
        __m128 y;
        switch (rounding) {
        case kRN: y = _mm_round_ss(x, x, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC); break;
        case kRP: y = _mm_round_ss(x, x, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC); break;
        case kRM: y = _mm_round_ss(x, x, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC); break;
        default: y = _mm_round_ss(x, x, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC); break;
        }
        H t = _mm_cvtss_f32(y);
        if (rounding == kRA && std::fabs(std::bit_cast<float>(a) - t) >= 0.5f)
            t += std::copysign(1.0f, t);
        r = std::bit_cast<U>(t);
    } else {
        const __m128d x = _mm_set_sd(std::bit_cast<double>(a));
        __m128d y;
        switch (rounding) {
        case kRN: y = _mm_round_sd(x, x, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC); break;
        case kRP: y = _mm_round_sd(x, x, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC); break;
        case kRM: y = _mm_round_sd(x, x, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC); break;
        default: y = _mm_round_sd(x, x, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC); break;
        }
        H t = _mm_cvtsd_f64(y);
        if (rounding == kRA && std::fabs(std::bit_cast<double>(a) - t) >= 0.5)
            t += std::copysign(1.0, t);
        r = std::bit_cast<U>(t);
    }
    if (exact && r != a)
        s.fpsr |= kIXC;
    return r;
}

// ---- FCVT{Z,N,P,M,A}{S,U} and the fixed-point forms: to a signed or unsigned ibits-bit integer, saturating
template <typename U>
u64 ToFixed(State& s, U a, int ibits, int fbits, bool uns, u8 rounding) {
    using I = Info<U>;
    const bool sign = I::Sign(a);
    const U e = I::Exp(a), frac = I::Frac(a);
    const u64 umax = ibits == 64 ? ~u64(0) : (u64(1) << ibits) - 1;
    const u64 smax = (u64(1) << (ibits - 1)) - 1;
    const auto saturate = [&] {
        s.fpsr |= kIOC;
        return uns ? (sign ? 0 : umax) : (sign ? u64(1) << (ibits - 1) : smax);
    };
    if (e == I::kExpMax) {
        if (frac) {
            s.fpsr |= kIOC;
            return 0; // NaN
        }
        return saturate();
    }
    if (e == 0) {
        if (frac == 0)
            return 0;
        if (FZ(s)) {
            s.fpsr |= kIDC;
            return 0;
        }
    }
    int exp;
    u64 mant;
    Decompose(a, exp, mant);
    exp += fbits;
    u64 mag;
    bool inexact = false;
    if (exp >= 0) {
        if (exp + (64 - std::countl_zero(mant)) > 64)
            return saturate(); // beyond 64 bits
        mag = mant << exp;
    } else {
        const int sh = -exp;
        u64 rem, half;
        if (sh >= 64) { // mant < 2^53, so the value is below one half
            mag = 0;
            rem = 1;
            half = 2;
        } else {
            mag = mant >> sh;
            rem = mant & ((u64(1) << sh) - 1);
            half = u64(1) << (sh - 1);
        }
        inexact = rem != 0;
        bool up = false;
        switch (rounding) {
        case kRN: up = rem > half || (rem == half && (mag & 1)); break;
        case kRP: up = inexact && !sign; break;
        case kRM: up = inexact && sign; break;
        case kRA: up = rem >= half; break;
        default: break;
        }
        mag += up;
    }
    u64 result;
    if (uns) {
        if (sign) {
            if (mag != 0)
                return saturate();
            result = 0; // a negative value that rounds to zero
        } else {
            if (mag > umax)
                return saturate();
            result = mag;
        }
    } else {
        if (mag > smax + (sign ? 1 : 0))
            return saturate();
        result = (sign ? u64(0) - mag : mag) & umax;
    }
    if (inexact)
        s.fpsr |= kIXC;
    return result;
}

// ---- half precision conversions (FCVT between half and single/double)
template <typename U>
U FromHalf(State& s, u16 h) {
    using I = Info<U>;
    using H16 = Info<u16>;
    const U sign_bits = (h >> 15) ? I::kSign : U(0);
    const u16 e = H16::Exp(h), frac = H16::Frac(h);
    const bool ahp = (s.fpcr & kFpcrAHP) != 0;
    if (e == 31 && !ahp) {
        if (frac == 0)
            return U(sign_bits | I::kInf);
        if (!(frac & 0x200))
            s.fpsr |= kIOC;
        if (s.fpcr & kFpcrDN)
            return I::kDefaultNaN;
        return U(sign_bits | I::kDefaultNaN | (U(frac & 0x1FF) << (I::kMant - 10)));
    }
    if (e == 0) {
        if (frac == 0)
            return sign_bits;
        const int top = 31 - std::countl_zero(u32(frac)); // value = frac * 2^-24
        const int vexp = top - 24;
        return U(sign_bits | (U(vexp + I::kBias) << I::kMant) |
                 ((U(frac) << (I::kMant - top)) & I::kMantMask));
    }
    return U(sign_bits | (U(int(e) - 15 + I::kBias) << I::kMant) | (U(frac) << (I::kMant - 10)));
}

template <typename U>
u16 ToHalf(State& s, U a, u8 rounding) {
    using I = Info<U>;
    const bool sign = I::Sign(a);
    const U e = I::Exp(a), frac = I::Frac(a);
    const bool ahp = (s.fpcr & kFpcrAHP) != 0;
    const u16 sign_bits = sign ? 0x8000 : 0;
    if (e == I::kExpMax) {
        if (frac) {
            if (!(frac & I::kQuietBit) || ahp)
                s.fpsr |= kIOC;
            if (ahp)
                return sign_bits;
            if (s.fpcr & kFpcrDN)
                return 0x7E00;
            return u16(sign_bits | 0x7E00 | ((frac >> (I::kMant - 10)) & 0x1FF));
        }
        if (ahp) {
            s.fpsr |= kIOC;
            return u16(sign_bits | 0x7FFF);
        }
        return u16(sign_bits | 0x7C00);
    }
    if (e == 0 && (frac == 0 || FZ(s))) {
        if (frac)
            s.fpsr |= kIDC;
        return sign_bits;
    }
    int exp;
    u64 mant;
    Decompose(a, exp, mant);
    return RoundPack<u16>(s, sign, exp, mant, rounding, false, ahp);
}

} // namespace

u32 FPRoundInt32(State& s, u32 a, u8 r, bool exact) { return RoundInt<u32>(s, a, r, exact); }
u64 FPRoundInt64(State& s, u64 a, u8 r, bool exact) { return RoundInt<u64>(s, a, r, exact); }
u32 FPRecipEstimate32(State& s, u32 a) { return RecipEstimate<u32>(s, a); }
u64 FPRecipEstimate64(State& s, u64 a) { return RecipEstimate<u64>(s, a); }
u32 FPRSqrtEstimate32(State& s, u32 a) { return RSqrtEstimate<u32>(s, a); }
u64 FPRSqrtEstimate64(State& s, u64 a) { return RSqrtEstimate<u64>(s, a); }
u32 FPRecipStepFused32(State& s, u32 a, u32 b) { return StepFused<u32, false>(s, a, b); }
u64 FPRecipStepFused64(State& s, u64 a, u64 b) { return StepFused<u64, false>(s, a, b); }
u32 FPRSqrtStepFused32(State& s, u32 a, u32 b) { return StepFused<u32, true>(s, a, b); }
u64 FPRSqrtStepFused64(State& s, u64 a, u64 b) { return StepFused<u64, true>(s, a, b); }
u32 FPRecipExponent32(State& s, u32 a) { return RecipExponent<u32>(s, a); }
u64 FPRecipExponent64(State& s, u64 a) { return RecipExponent<u64>(s, a); }
u32 FPHalfToSingle(State& s, u16 a, u8) { return FromHalf<u32>(s, a); } // always exact
u64 FPHalfToDouble(State& s, u16 a, u8) { return FromHalf<u64>(s, a); }
u16 FPSingleToHalf(State& s, u32 a, u8 r) { return ToHalf<u32>(s, a, r); }
u16 FPDoubleToHalf(State& s, u64 a, u8 r) { return ToHalf<u64>(s, a, r); }
u64 FPToFixed32(State& s, u32 a, int ibits, u8 fbits, bool uns, u8 r) {
    // FCVTZS/FCVTZU to 32 bits, the common case: the host's truncating conversion, for normal |a| < 2^31
    // (normal |a| < 2^31, or 0 <= a < 2^32 unsigned).
    if (ibits == 32 && fbits == 0 && r == kRZ) {
        const u32 e = (a >> 23) & 0xFF;
        const float f = std::bit_cast<float>(a);
        if (!uns && e - 1 < 157) {
            const s32 v = _mm_cvttss_si32(_mm_set_ss(f));
            if (float(v) != f)
                s.fpsr |= kIXC;
            return u32(v);
        }
        if (uns && !(a >> 31) && e - 1 < 158) {
            const s64 v = _mm_cvttss_si64(_mm_set_ss(f));
            if (float(v) != f)
                s.fpsr |= kIXC;
            return u32(v);
        }
    }
    return ToFixed<u32>(s, a, ibits, fbits, uns, r);
}
u64 FPToFixed64(State& s, u64 a, int ibits, u8 fbits, bool uns, u8 r) {
    return ToFixed<u64>(s, a, ibits, fbits, uns, r);
}

// ---- AES rounds (AESE / AESD after the key XOR, AESMC, AESIMC): AES-NI
namespace {
// through memory, not V128's members: works with V128 as two u64 or as one __m128i (16 bytes, low half first)
static_assert(sizeof(V128) == 16);
__m128i ToM(V128 a) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(&a)); }
V128 FromM(__m128i x) {
    V128 r;
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&r), x);
    return r;
}
} // namespace
V128 AESEncryptSingleRound(V128 a) { return FromM(_mm_aesenclast_si128(ToM(a), _mm_setzero_si128())); }
V128 AESDecryptSingleRound(V128 a) { return FromM(_mm_aesdeclast_si128(ToM(a), _mm_setzero_si128())); }
V128 AESMixColumns(V128 a) {
    // aesdeclast undoes ShiftRows + SubBytes, aesenc redoes them and adds MixColumns
    return FromM(_mm_aesenc_si128(_mm_aesdeclast_si128(ToM(a), _mm_setzero_si128()), _mm_setzero_si128()));
}
V128 AESInverseMixColumns(V128 a) { return FromM(_mm_aesimc_si128(ToM(a))); }

} // namespace recomp
