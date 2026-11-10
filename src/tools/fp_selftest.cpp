// fp_selftest: compares the native floating-point helpers of recompiled code (core/cpu/recomp/recomp_fp.cpp) with
// dynarmic's soft-float reference, bit for bit, results and FPSR flags, over exhaustive single-precision inputs
// and edge-weighted random inputs, in every FPCR rounding mode with and without FZ / DN (/ AHP).
//
//   fp_selftest [quick]      quick: random inputs only (seconds); default adds the exhaustive 2^32 passes
//
// Known, intended differences (counted, not failures):
//   - FCVT*U of a negative value that rounds to zero: ARM raises Inexact, dynarmic Invalid Operation.
//   - FRECPS / FRSQRTS: the native code raises only IOC / DZC / IDC (Inexact / Overflow are not tracked, like the
//     other native FP ops).
// Where dynarmic itself is wrong (deep cancellation in its soft fused multiply-add; FPToFixed saturating an
// in-range value), a mismatch counts as "dynarmic wrong" only if native equals an independent exact reference.
#include <atomic>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <dynarmic/common/crypto/aes.h>
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

using namespace recomp;
namespace FP = Dynarmic::FP;

namespace {

constexpr u32 kIOC = 1, kDZC = 2, kIXC = 16, kIDC = 128;

// MXCSR as native code runs with it: FPCR rounding mode and FZ (FTZ + DAZ), exceptions masked.
void SetGuestMxcsr(u32 fpcr) {
    static constexpr u32 kRc[4] = {0x0000, 0x4000, 0x2000, 0x6000};
    _mm_setcsr(0x1F80 | kRc[(fpcr >> 22) & 3] | ((fpcr >> 24) & 1 ? 0x8040u : 0u));
}

struct Stats {
    std::atomic<u64> checked{0}, failed{0}, known{0};
    std::atomic<u64> dyn_bug{0}; // dynarmic wrong, native equal to an independent exact reference
};

std::atomic<int> g_printed{0};
struct In {
    u64 a, b = 0;
    int extra = -1; // rounding argument
};
void Report(const char* what, u32 fpcr, In in, u64 got, u32 got_fpsr, u64 want, u32 want_fpsr) {
    if (g_printed.fetch_add(1) < 40)
        std::printf("MISMATCH %s fpcr=%08x in=%llx,%llx r%d: native %llx fpsr %02x, dynarmic %llx fpsr %02x\n", what,
                    fpcr, (unsigned long long)in.a, (unsigned long long)in.b, in.extra, (unsigned long long)got,
                    got_fpsr, (unsigned long long)want, want_fpsr);
}

// Compares one call: f_native(State&) -> u64 against f_ref(FPCR, FPSR&) -> u64. flag_mask: flags compared.
template <typename N, typename R>
bool Check(Stats& st, const char* what, u32 fpcr, In in, N&& native, R&& ref, u32 flag_mask = ~0u) {
    State s{};
    s.fpcr = fpcr;
    s.fpsr = 0;
    const u64 got = native(s);
    FP::FPSR fpsr{0};
    const u64 want = ref(FP::FPCR{fpcr}, fpsr);
    st.checked.fetch_add(1, std::memory_order_relaxed);
    if (got != want || ((s.fpsr ^ fpsr.Value()) & flag_mask)) {
        st.failed.fetch_add(1, std::memory_order_relaxed);
        Report(what, fpcr, in, got, s.fpsr, want, fpsr.Value());
        return false;
    }
    return true;
}

// Edge-weighted random bits of a format with kMant fraction bits and kExpBits exponent bits.
template <typename U, int kMant, int kExpBits>
struct Gen {
    std::mt19937_64 rng;
    explicit Gen(u64 seed) : rng(seed) {}
    U operator()() {
        constexpr U kExpMax = U((1u << kExpBits) - 1);
        constexpr int kBias = (1 << (kExpBits - 1)) - 1;
        const u64 r = rng();
        U e;
        switch (r % 8) {
        case 0: e = 0; break;
        case 1: e = kExpMax; break;
        case 2: e = U(1 + (r >> 8) % 2); break;
        case 3: e = U(kExpMax - 1 - (r >> 8) % 3); break;
        case 4: e = U(kBias - 40 + (r >> 8) % 80); break;
        default: e = U((r >> 8) % (u64(kExpMax) + 1)); break;
        }
        const u64 r2 = rng();
        U m;
        switch (r2 % 6) {
        case 0: m = 0; break;
        case 1: m = U((U(1) << kMant) - 1); break;
        case 2: m = U(U(1) << (kMant - 1)); break;
        case 3: m = U(1 + (r2 >> 8) % 4); break;
        default: m = U(rng() & ((U(1) << kMant) - 1)); break;
        }
        const U sign = U((rng() & 1) << (sizeof(U) * 8 - 1));
        return U(sign | (U(e) << kMant) | m);
    }
};

std::vector<u32> FpcrConfigs(bool with_ahp) {
    std::vector<u32> v;
    for (u32 rm = 0; rm < 4; ++rm)
        for (u32 fz = 0; fz < 2; ++fz)
            for (u32 dn = 0; dn < 2; ++dn)
                for (u32 ahp = 0; ahp < (with_ahp ? 2u : 1u); ++ahp)
                    v.push_back((rm << 22) | (fz << 24) | (dn << 25) | (ahp << 26));
    return v;
}

// Runs body(begin, end) over [0, 2^32) on 4 threads.
template <typename F>
void Exhaustive32(F&& body) {
    constexpr int kThreads = 4;
    std::vector<std::thread> t;
    const u64 n = u64(1) << 32;
    for (int i = 0; i < kThreads; ++i)
        t.emplace_back([&, i] { body(n * i / kThreads, n * (i + 1) / kThreads); });
    for (auto& x : t)
        x.join();
}

// ---- unary single-precision functions, exhaustively for a list of FPCRs
void ExhaustiveUnary32(Stats& st, const char* what, const std::vector<u32>& fpcrs,
                       u32 (*native)(State&, u32), u32 (*ref)(u32, FP::FPCR, FP::FPSR&)) {
    for (u32 fpcr : fpcrs) {
        Exhaustive32([&](u64 b, u64 e) {
            SetGuestMxcsr(fpcr);
            for (u64 i = b; i < e; ++i) {
                const u32 a = u32(i);
                Check(st, what, fpcr, In{u64(a)}, [&](State& s) { return u64(native(s, a)); },
                      [&](FP::FPCR c, FP::FPSR& f) { return u64(ref(a, c, f)); });
            }
        });
        std::printf("  %s fpcr=%08x done\n", what, fpcr);
    }
}

u32 NRecipEst32(State& s, u32 a) { return FPRecipEstimate32(s, a); }
u32 RRecipEst32(u32 a, FP::FPCR c, FP::FPSR& f) { return FP::FPRecipEstimate<u32>(a, c, f); }
u32 NRSqrtEst32(State& s, u32 a) { return FPRSqrtEstimate32(s, a); }
u32 RRSqrtEst32(u32 a, FP::FPCR c, FP::FPSR& f) { return FP::FPRSqrtEstimate<u32>(a, c, f); }
u32 NRecipExp32(State& s, u32 a) { return FPRecipExponent32(s, a); }
u32 RRecipExp32(u32 a, FP::FPCR c, FP::FPSR& f) { return FP::FPRecipExponent<u32>(a, c, f); }

// ---- independent exact references, used when native and dynarmic disagree (dynarmic's soft-float has bugs too:
// its fused multiply-add loses the exponent on deep cancellation, and FPToFixed saturates some in-range values).

// FPToFixed by the host's correctly rounded nearbyint under fesetround: normal or zero inputs only.
template <typename U>
bool ExactToFixed(U a, int ibits, int fbits, bool uns, u8 r, u64& out, u32& fpsr) {
    using H = std::conditional_t<sizeof(U) == 4, float, double>;
    const H x = std::bit_cast<H>(a);
    if (!std::isfinite(x) || (x != 0 && !std::isnormal(x)))
        return false;
    const double scaled = std::ldexp(double(x), fbits); // exact: double holds every float, and |fbits| < 64
    if (!std::isfinite(scaled))
        return false;
    double rounded;
    if (r == 4) {
        rounded = std::trunc(scaled);
        if (std::fabs(scaled - rounded) >= 0.5)
            rounded += std::copysign(1.0, scaled);
    } else {
        static constexpr int kMode[4] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
        const int old = std::fegetround();
        std::fesetround(kMode[r]);
        volatile double v = scaled;
        rounded = std::nearbyint(v);
        std::fesetround(old);
    }
    const double hi = std::ldexp(1.0, uns ? ibits : ibits - 1); // exclusive
    const double lo = uns ? 0.0 : -hi;
    const u64 mask = ibits == 64 ? ~u64(0) : (u64(1) << ibits) - 1;
    fpsr = 0;
    if (rounded >= hi) {
        out = uns ? mask : (mask >> 1);
        fpsr = kIOC;
    } else if (rounded < lo) {
        out = uns ? 0 : ((mask >> 1) + 1);
        fpsr = kIOC;
    } else {
        out = (uns ? u64(rounded) : u64(s64(rounded))) & mask;
        if (rounded != scaled)
            fpsr = kIXC;
    }
    return true;
}

// FRECPS (c - a*b) / FRSQRTS ((c - a*b) / 2) on deep cancellation, without FMA: a*b split exactly (Dekker), then
// c - hi is exact (Sterbenz) and (c - hi) - lo is the one correctly rounded operation, in the guest rounding mode.
template <typename U, bool kRSqrt>
bool ExactStep(U a, U b, u32 fpcr, u64& out) {
    using H = std::conditional_t<sizeof(U) == 4, float, double>;
    const H fa = std::bit_cast<H>(a), fb = std::bit_cast<H>(b);
    if (!std::isfinite(fa) || !std::isfinite(fb) || fa == 0 || fb == 0)
        return false;
    if (((fpcr >> 24) & 1) && (!std::isnormal(fa) || !std::isnormal(fb)))
        return false; // FZ flushes the input: not a cancellation case
    const double c = kRSqrt ? 3.0 : 2.0;
    volatile double res;
    if constexpr (sizeof(U) == 4) {
        _mm_setcsr(0x1F80);
        const double p = double(fa) * double(fb); // exact: 48 significant bits
        if (!(p >= c / 2 && p <= 2 * c))
            return false;
        const double d = c - p; // exact (Sterbenz)
        SetGuestMxcsr(fpcr);
        volatile double dv = kRSqrt ? d * 0.5 : d;
        volatile float f = float(dv);
        if (!std::isnormal(float(f)))
            return false;
        out = std::bit_cast<u32>(float(f));
        return true;
    } else {
        int e;
        std::frexp(fa, &e);
        const double a1 = std::ldexp(fa, -e), b1 = std::ldexp(fb, e); // exact rescale, a1*b1 == a*b
        if (!std::isnormal(b1))
            return false;
        _mm_setcsr(0x1F80);
        const double hi = a1 * b1;
        if (!(hi >= c / 2 && hi <= 2 * c))
            return false;
        auto split = [](double x, double& h, double& l) {
            const double t = x * 134217729.0; // 2^27 + 1
            h = t - (t - x);
            l = x - h;
        };
        double ah, al, bh, bl;
        split(a1, ah, al);
        split(b1, bh, bl);
        const double lo = ((ah * bh - hi) + ah * bl + al * bh) + al * bl; // a1*b1 == hi + lo exactly
        const double d = c - hi;                                          // exact (Sterbenz)
        SetGuestMxcsr(fpcr);
        volatile double dv = d, lv = lo;
        res = dv - lv;
        if (!std::isnormal(double(res)))
            return false;
        res = kRSqrt ? res * 0.5 : double(res);
        if (!std::isnormal(double(res)))
            return false;
        out = std::bit_cast<u64>(double(res));
        return true;
    }
}

bool KnownToFixedDiff(bool uns, u64 a_bits, int bits, u32 native_fpsr, u32 ref_fpsr, u64 got, u64 want) {
    const bool sign = (a_bits >> (bits - 1)) & 1;
    return uns && sign && got == 0 && want == 0 && (native_fpsr & ~kIXC) == (ref_fpsr & ~kIOC) &&
           (ref_fpsr & kIOC) && !(native_fpsr & kIOC);
}

template <typename U>
void CheckToFixed(Stats& st, u32 fpcr, U a, int ibits, int fbits, bool uns, u8 r) {
    State s{};
    s.fpcr = fpcr;
    const u64 got = sizeof(U) == 4 ? FPToFixed32(s, u32(a), ibits, u8(fbits), uns, r)
                                   : FPToFixed64(s, u64(a), ibits, u8(fbits), uns, r);
    FP::FPSR f{0};
    const u64 want = FP::FPToFixed<U>(size_t(ibits), a, size_t(fbits), uns, FP::FPCR{fpcr},
                                      static_cast<FP::RoundingMode>(r), f);
    st.checked.fetch_add(1, std::memory_order_relaxed);
    if (got == want && s.fpsr == f.Value())
        return;
    if (KnownToFixedDiff(uns, a, int(sizeof(U) * 8), s.fpsr, f.Value(), got, want)) {
        st.known.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    u64 ex;
    u32 ex_fpsr;
    if (ExactToFixed<U>(a, ibits, fbits, uns, r, ex, ex_fpsr) && ex == got && ex_fpsr == s.fpsr) {
        st.dyn_bug.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    st.failed.fetch_add(1, std::memory_order_relaxed);
    char what[64];
    std::snprintf(what, sizeof(what), "ToFixed%d(i%d f%d %s r%u)", int(sizeof(U) * 8), ibits, fbits, uns ? "u" : "s",
                  r);
    Report(what, fpcr, In{u64(a), u64(fbits), r}, got, s.fpsr, want, f.Value());
}

template <typename U>
void CheckRoundInt(Stats& st, u32 fpcr, U a, u8 r, bool exact) {
    Check(st, sizeof(U) == 4 ? "RoundInt32" : "RoundInt64", fpcr, In{u64(a), 0, r},
          [&](State& s) { return sizeof(U) == 4 ? u64(FPRoundInt32(s, u32(a), r, exact)) : FPRoundInt64(s, u64(a), r, exact); },
          [&](FP::FPCR c, FP::FPSR& f) { return u64(FP::FPRoundInt<U>(a, c, static_cast<FP::RoundingMode>(r), exact, f)); });
}

template <typename U, bool kRSqrt>
void CheckStep(Stats& st, u32 fpcr, U a, U b) {
    State s{};
    s.fpcr = fpcr;
    u64 got;
    if constexpr (sizeof(U) == 4)
        got = kRSqrt ? FPRSqrtStepFused32(s, a, b) : FPRecipStepFused32(s, a, b);
    else
        got = kRSqrt ? FPRSqrtStepFused64(s, a, b) : FPRecipStepFused64(s, a, b);
    FP::FPSR f{0};
    const u64 want = kRSqrt ? FP::FPRSqrtStepFused<U>(a, b, FP::FPCR{fpcr}, f) : FP::FPRecipStepFused<U>(a, b, FP::FPCR{fpcr}, f);
    st.checked.fetch_add(1, std::memory_order_relaxed);
    constexpr u32 kMask = kIOC | kDZC | kIDC;
    if (got == want && !((s.fpsr ^ f.Value()) & kMask))
        return;
    u64 ex;
    const bool ok = ExactStep<U, kRSqrt>(a, b, fpcr, ex) && ex == got && !((s.fpsr ^ f.Value()) & kMask);
    SetGuestMxcsr(fpcr);
    if (ok) {
        st.dyn_bug.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    st.failed.fetch_add(1, std::memory_order_relaxed);
    Report(kRSqrt ? "RSqrtStepFused" : "RecipStepFused", fpcr, In{u64(a), u64(b)}, got, s.fpsr, want, f.Value());
}

// b chosen so that a*b is close to 2 or 3 (cancellation), or huge (overflow of the intermediate)
template <typename U, typename H>
U Partner(std::mt19937_64& rng, U a) {
    const H fa = std::bit_cast<H>(a);
    if (!(fa == fa) || fa == 0 || std::isinf(fa))
        return a;
    static constexpr H kTargets[] = {H(2), H(3), H(-2), H(-3)};
    const u64 k = rng() % 6;
    H target = k < 4 ? kTargets[k] : (k == 4 ? std::numeric_limits<H>::max() : -std::numeric_limits<H>::max());
    U b = std::bit_cast<U>(H(target / fa));
    const int delta = int(rng() % 9) - 4;
    return U(b + U(delta));
}

void RandomTests(Stats& st, u64 n) {
    const auto fpcrs = FpcrConfigs(false);
    const auto fpcrs_ahp = FpcrConfigs(true);
    std::vector<std::thread> th;
    for (int t = 0; t < 4; ++t)
        th.emplace_back([&, t] {
            std::mt19937_64 rng(1234 + t);
            Gen<u32, 23, 8> g32(77 + t);
            Gen<u64, 52, 11> g64(99 + t);
            Gen<u16, 10, 5> g16(55 + t);
            for (u64 i = 0; i < n; ++i) {
                const u32 fpcr = fpcrs[i % fpcrs.size()];
                SetGuestMxcsr(fpcr);
                const u32 a32 = g32();
                const u64 a64 = g64();
                const u8 r = u8(rng() % 5);
                // unary 64-bit
                Check(st, "RecipEstimate64", fpcr, In{u64(a64)}, [&](State& s) { return FPRecipEstimate64(s, a64); },
                      [&](FP::FPCR c, FP::FPSR& f) { return u64(FP::FPRecipEstimate<u64>(a64, c, f)); });
                Check(st, "RSqrtEstimate64", fpcr, In{u64(a64)}, [&](State& s) { return FPRSqrtEstimate64(s, a64); },
                      [&](FP::FPCR c, FP::FPSR& f) { return u64(FP::FPRSqrtEstimate<u64>(a64, c, f)); });
                Check(st, "RecipExponent64", fpcr, In{u64(a64)}, [&](State& s) { return FPRecipExponent64(s, a64); },
                      [&](FP::FPCR c, FP::FPSR& f) { return u64(FP::FPRecipExponent<u64>(a64, c, f)); });
                Check(st, "RecipEstimate32", fpcr, In{u64(a32)}, [&](State& s) { return u64(FPRecipEstimate32(s, a32)); },
                      [&](FP::FPCR c, FP::FPSR& f) { return u64(FP::FPRecipEstimate<u32>(a32, c, f)); });
                Check(st, "RSqrtEstimate32", fpcr, In{u64(a32)}, [&](State& s) { return u64(FPRSqrtEstimate32(s, a32)); },
                      [&](FP::FPCR c, FP::FPSR& f) { return u64(FP::FPRSqrtEstimate<u32>(a32, c, f)); });
                CheckRoundInt<u32>(st, fpcr, a32, r, (i & 1) != 0);
                CheckRoundInt<u64>(st, fpcr, a64, r, (i & 2) != 0);
                // to fixed point
                static constexpr int kIbits[] = {16, 32, 64};
                const int ibits = kIbits[rng() % 3];
                const int fbits = (rng() & 1) ? 0 : int(rng() % (ibits + 1));
                const bool uns = rng() & 1;
                CheckToFixed<u32>(st, fpcr, a32, ibits, fbits, uns, r);
                CheckToFixed<u64>(st, fpcr, a64, ibits, fbits, uns, r);
                // fused steps
                const u32 b32 = (rng() & 1) ? g32() : Partner<u32, float>(rng, a32);
                const u64 b64 = (rng() & 1) ? g64() : Partner<u64, double>(rng, a64);
                CheckStep<u32, false>(st, fpcr, a32, b32);
                CheckStep<u32, true>(st, fpcr, a32, b32);
                CheckStep<u64, false>(st, fpcr, a64, b64);
                CheckStep<u64, true>(st, fpcr, a64, b64);
                // half precision
                const u32 fpcr_h = fpcrs_ahp[i % fpcrs_ahp.size()];
                const u8 rh = u8(rng() % 4);
                const u16 h = g16();
                Check(st, "HalfToSingle", fpcr_h, In{u64(h)}, [&](State& s) { return u64(FPHalfToSingle(s, h, rh)); },
                      [&](FP::FPCR c, FP::FPSR& f) {
                          return u64(FP::FPConvert<u32, u16>(h, c, static_cast<FP::RoundingMode>(rh), f));
                      });
                Check(st, "HalfToDouble", fpcr_h, In{u64(h)}, [&](State& s) { return FPHalfToDouble(s, h, rh); },
                      [&](FP::FPCR c, FP::FPSR& f) {
                          return u64(FP::FPConvert<u64, u16>(h, c, static_cast<FP::RoundingMode>(rh), f));
                      });
                // singles and doubles in and around the half range
                const u32 s_near = (a32 & 0x807FFFFFu) | (u32(127 - 30 + rng() % 50) << 23);
                const u32 sv = (rng() & 1) ? a32 : s_near;
                const u64 d_near = (a64 & 0x800FFFFFFFFFFFFFull) | (u64(1023 - 30 + rng() % 50) << 52);
                const u64 dv = (rng() & 1) ? a64 : d_near;
                Check(st, "SingleToHalf", fpcr_h, In{u64(sv)}, [&](State& s) { return u64(FPSingleToHalf(s, sv, rh)); },
                      [&](FP::FPCR c, FP::FPSR& f) {
                          return u64(FP::FPConvert<u16, u32>(sv, c, static_cast<FP::RoundingMode>(rh), f));
                      });
                Check(st, "DoubleToHalf", fpcr_h, In{u64(dv)}, [&](State& s) { return u64(FPDoubleToHalf(s, dv, rh)); },
                      [&](FP::FPCR c, FP::FPSR& f) {
                          return u64(FP::FPConvert<u16, u64>(dv, c, static_cast<FP::RoundingMode>(rh), f));
                      });
                // AES
                if ((i & 7) == 0) {
                    const V128 v{rng(), rng()};
                    using AES = Dynarmic::Common::Crypto::AES::State;
                    const auto ref = [&](void (*fn)(AES&, const AES&)) {
                        AES in, out;
                        std::memcpy(in.data(), &v, 16);
                        fn(out, in);
                        V128 o;
                        std::memcpy(&o, out.data(), 16);
                        return o;
                    };
                    const V128 n[4] = {AESEncryptSingleRound(v), AESDecryptSingleRound(v), AESMixColumns(v),
                                       AESInverseMixColumns(v)};
                    const V128 w[4] = {ref(Dynarmic::Common::Crypto::AES::EncryptSingleRound),
                                       ref(Dynarmic::Common::Crypto::AES::DecryptSingleRound),
                                       ref(Dynarmic::Common::Crypto::AES::MixColumns),
                                       ref(Dynarmic::Common::Crypto::AES::InverseMixColumns)};
                    for (int k = 0; k < 4; ++k) {
                        st.checked.fetch_add(1, std::memory_order_relaxed);
                        if (n[k].lo != w[k].lo || n[k].hi != w[k].hi) {
                            st.failed.fetch_add(1, std::memory_order_relaxed);
                            Report("AES", 0, In{v.lo, v.hi, k}, n[k].lo, 0, w[k].lo, 0);
                        }
                    }
                }
            }
        });
    for (auto& x : th)
        x.join();
}

void ExhaustiveHalf(Stats& st) {
    for (u32 fpcr : FpcrConfigs(true)) {
        SetGuestMxcsr(fpcr);
        for (u32 h = 0; h < 0x10000; ++h)
            for (u8 r = 0; r < 4; ++r) {
                Check(st, "HalfToSingle", fpcr, In{u64(h)}, [&](State& s) { return u64(FPHalfToSingle(s, u16(h), r)); },
                      [&](FP::FPCR c, FP::FPSR& f) {
                          return u64(FP::FPConvert<u32, u16>(u16(h), c, static_cast<FP::RoundingMode>(r), f));
                      });
                Check(st, "HalfToDouble", fpcr, In{u64(h)}, [&](State& s) { return FPHalfToDouble(s, u16(h), r); },
                      [&](FP::FPCR c, FP::FPSR& f) {
                          return u64(FP::FPConvert<u64, u16>(u16(h), c, static_cast<FP::RoundingMode>(r), f));
                      });
            }
    }
}

void ExhaustiveFixedAndRound(Stats& st) {
    // the configurations the game runs: FPCR 0 and FZ, conversions to 32-bit integers, every rounding
    for (u32 fpcr : {0u, 1u << 24}) {
        for (u8 r = 0; r < 5; ++r) {
            for (int uns = 0; uns < 2; ++uns) {
                Exhaustive32([&](u64 b, u64 e) {
                    SetGuestMxcsr(fpcr);
                    for (u64 i = b; i < e; ++i)
                        CheckToFixed<u32>(st, fpcr, u32(i), 32, 0, uns != 0, r);
                });
                std::printf("  ToFixed32(i32 f0 %s r%u) fpcr=%08x done\n", uns ? "u" : "s", r, fpcr);
            }
            Exhaustive32([&](u64 b, u64 e) {
                SetGuestMxcsr(fpcr);
                for (u64 i = b; i < e; ++i)
                    CheckRoundInt<u32>(st, fpcr, u32(i), r, true);
            });
            std::printf("  RoundInt32(r%u) fpcr=%08x done\n", r, fpcr);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const bool quick = argc > 1 && std::strcmp(argv[1], "quick") == 0;
    Stats st;
    std::printf("random tests...\n");
    RandomTests(st, quick ? 200'000 : 4'000'000);
    std::printf("exhaustive half precision...\n");
    ExhaustiveHalf(st);
    if (!quick) {
        std::printf("exhaustive single precision...\n");
        const std::vector<u32> main_fpcrs = {0u, 1u << 24, 1u << 25, (1u << 24) | (1u << 25)};
        ExhaustiveUnary32(st, "RSqrtEstimate32", main_fpcrs, NRSqrtEst32, RRSqrtEst32);
        ExhaustiveUnary32(st, "RecipEstimate32", main_fpcrs, NRecipEst32, RRecipEst32);
        ExhaustiveUnary32(st, "RecipExponent32", main_fpcrs, NRecipExp32, RRecipExp32);
        ExhaustiveFixedAndRound(st);
    }
    std::printf("checked %llu, failed %llu, known ARM-vs-dynarmic flag differences %llu, dynarmic wrong (native "
                "matches an exact reference) %llu\n",
                (unsigned long long)st.checked.load(), (unsigned long long)st.failed.load(),
                (unsigned long long)st.known.load(), (unsigned long long)st.dyn_bug.load());
    return st.failed.load() ? 1 : 0;
}
