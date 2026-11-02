#pragma once
// Runtime support for statically recompiled guest code (see src/tools/recomp.cpp).
//
// The generator translates each guest basic block with dynarmic's A64 frontend and emits one C++ statement per IR
// microinstruction. Generic IR opcodes become calls to the inline functions in namespace `op` below (same names as
// the IR opcodes), so the semantics here must match dynarmic's x64 backend exactly.
#include <algorithm>
#include <atomic>
#include <bit>
#include <limits>
#include <type_traits>
#include <cmath>
#include <immintrin.h>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <nmmintrin.h>

// Generated functions are huge, and MSVC stops inlining into them: force it for every op.
#define RC_INLINE __forceinline

namespace recomp {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

struct V128 {
    u64 lo, hi;
};

// Guest register file for native code. Lives in the owning cpu::Core; copied from/to dynarmic at transitions.
struct State {
    u64 x[31];
    u64 sp;
    u64 pc; // on return from a native function: the next guest pc
    u32 nzcv;
    u32 fpcr, fpsr;
    u32 pad;
    alignas(16) V128 v[32];
    u64 tpidr, tpidrro;
    u8* mem;                  // fastmem base (guest address 0)
    volatile u32* halt;       // nonzero: leave native code at the next check
    u64 icount;               // guest instructions executed natively
    void* core;               // owning core (runtime callbacks)
    u64 stack_limit;          // host stack floor: functions refuse to run below it (deep guest recursion)
};

using Fn = void (*)(State&);

// ---- runtime services (recomp_runtime.cpp)
void Svc(State& s, u32 imm);
void SetFpcr(State& s, u32 v); // MSR FPCR: also re-derives the host MXCSR (rounding mode, flush-to-zero)
void Undefined(State& s);      // an undecodable instruction at s.pc: the guest's undefined-instruction exception
u64 Cntpct(State& s);
void Barrier();
u8 ExRead8(State& s, u64 a);
u16 ExRead16(State& s, u64 a);
u32 ExRead32(State& s, u64 a);
u64 ExRead64(State& s, u64 a);
V128 ExRead128(State& s, u64 a);
u32 ExWrite8(State& s, u64 a, u8 v);
u32 ExWrite16(State& s, u64 a, u16 v);
u32 ExWrite32(State& s, u64 a, u32 v);
u32 ExWrite64(State& s, u64 a, u64 v);
u32 ExWrite128(State& s, u64 a, V128 v);
void ClearExclusive(State& s);
// Soft-float helpers with exact ARM semantics (dynarmic's FP library, recomp_fp.cpp)
u32 FPRoundInt32(State& s, u32 a, u8 rounding, bool exact);
u64 FPRoundInt64(State& s, u64 a, u8 rounding, bool exact);
u32 FPRecipEstimate32(State& s, u32 a);
u64 FPRecipEstimate64(State& s, u64 a);
u32 FPRSqrtEstimate32(State& s, u32 a);
u64 FPRSqrtEstimate64(State& s, u64 a);
u32 FPRecipStepFused32(State& s, u32 a, u32 b);
u64 FPRecipStepFused64(State& s, u64 a, u64 b);
u32 FPRSqrtStepFused32(State& s, u32 a, u32 b);
u64 FPRSqrtStepFused64(State& s, u64 a, u64 b);
u32 FPRecipExponent32(State& s, u32 a);
u64 FPRecipExponent64(State& s, u64 a);
u32 FPHalfToSingle(State& s, u16 a, u8 rounding);
u64 FPHalfToDouble(State& s, u16 a, u8 rounding);
u16 FPSingleToHalf(State& s, u32 a, u8 rounding);
u16 FPDoubleToHalf(State& s, u64 a, u8 rounding);
u64 FPToFixed32(State& s, u32 a, int ibits, u8 fbits, bool is_unsigned, u8 rounding);
u64 FPToFixed64(State& s, u64 a, int ibits, u8 fbits, bool is_unsigned, u8 rounding);
RC_INLINE u16 FPSingleToFixedS16(State& s, u32 a, u8 f, u8 r) { return u16(FPToFixed32(s, a, 16, f, false, r)); }
RC_INLINE u32 FPSingleToFixedS32(State& s, u32 a, u8 f, u8 r) { return u32(FPToFixed32(s, a, 32, f, false, r)); }
RC_INLINE u64 FPSingleToFixedS64(State& s, u32 a, u8 f, u8 r) { return u64(FPToFixed32(s, a, 64, f, false, r)); }
RC_INLINE u16 FPSingleToFixedU16(State& s, u32 a, u8 f, u8 r) { return u16(FPToFixed32(s, a, 16, f, true, r)); }
RC_INLINE u32 FPSingleToFixedU32(State& s, u32 a, u8 f, u8 r) { return u32(FPToFixed32(s, a, 32, f, true, r)); }
RC_INLINE u64 FPSingleToFixedU64(State& s, u32 a, u8 f, u8 r) { return u64(FPToFixed32(s, a, 64, f, true, r)); }
RC_INLINE u16 FPDoubleToFixedS16(State& s, u64 a, u8 f, u8 r) { return u16(FPToFixed64(s, a, 16, f, false, r)); }
RC_INLINE u32 FPDoubleToFixedS32(State& s, u64 a, u8 f, u8 r) { return u32(FPToFixed64(s, a, 32, f, false, r)); }
RC_INLINE u64 FPDoubleToFixedS64(State& s, u64 a, u8 f, u8 r) { return u64(FPToFixed64(s, a, 64, f, false, r)); }
RC_INLINE u16 FPDoubleToFixedU16(State& s, u64 a, u8 f, u8 r) { return u16(FPToFixed64(s, a, 16, f, true, r)); }
RC_INLINE u32 FPDoubleToFixedU32(State& s, u64 a, u8 f, u8 r) { return u32(FPToFixed64(s, a, 32, f, true, r)); }
RC_INLINE u64 FPDoubleToFixedU64(State& s, u64 a, u8 f, u8 r) { return u64(FPToFixed64(s, a, 64, f, true, r)); }
// C library routines run by the host (generated for the SDK's memcpy, memset...): AAPCS64 arguments in x0..x2,
// result in x0.
void Libc_memcpy(State& s);
void Libc_memmove(State& s);
void Libc_memset(State& s);
void Libc_memcmp(State& s);
void Libc_strlen(State& s);
void Libc_strcmp(State& s);
void Libc_strncmp(State& s);
void Libc_memchr(State& s);
void Libc_strchr(State& s);
// Runs the native function at s.pc if there is one (indirect call / tail jump); otherwise returns with s.pc unchanged.
void Dispatch(State& s);

// Emitted table (generated recomp_table.cpp, or recomp_stub.cpp when nothing was generated).
struct FuncEntry {
    u64 entry;
    Fn fn;
    u32 range_first, range_count; // into ranges: [start, end) guest words this function was compiled from
    u64 hash;                     // FNV-1a over those words; mismatch at runtime (patched code) disables it
};
struct CodeRange {
    u64 start, end;
};
// Call-return points inside a function: native code can be re-entered there (f is called with s.pc = pc).
struct ResumeEntry {
    u64 pc;
    u32 func;
};
struct Table {
    const FuncEntry* funcs;
    u32 num_funcs;
    const CodeRange* ranges;
    u32 num_ranges;
    u8* const* ok; // per function: flag in its shard, 1 = verified and enabled (written by the runtime)
    const ResumeEntry* resumes;
    u32 num_resumes;
};
const Table& GetTable();

// ---- memory
template <typename T>
RC_INLINE T Rd(State& s, u64 a) {
    return *reinterpret_cast<volatile T*>(s.mem + a);
}
template <typename T>
RC_INLINE void Wr(State& s, u64 a, T v) {
    *reinterpret_cast<volatile T*>(s.mem + a) = v;
}
RC_INLINE V128 Rd128(State& s, u64 a) { return {Rd<u64>(s, a), Rd<u64>(s, a + 8)}; }
RC_INLINE void Wr128(State& s, u64 a, V128 v) {
    Wr<u64>(s, a, v.lo);
    Wr<u64>(s, a + 8, v.hi);
}
// Release stores (STLR): a locked exchange also orders them against later loads, as ARM's RCsc LDAR requires.
RC_INLINE void WrOrdered(State& s, u64 a, u8 v) { _InterlockedExchange8(reinterpret_cast<volatile char*>(s.mem + a), char(v)); }
RC_INLINE void WrOrdered(State& s, u64 a, u16 v) { _InterlockedExchange16(reinterpret_cast<volatile short*>(s.mem + a), short(v)); }
RC_INLINE void WrOrdered(State& s, u64 a, u32 v) { _InterlockedExchange(reinterpret_cast<volatile long*>(s.mem + a), long(v)); }
RC_INLINE void WrOrdered(State& s, u64 a, u64 v) {
    _InterlockedExchange64(reinterpret_cast<volatile long long*>(s.mem + a), (long long)v);
}

constexpr u32 kN = 1u << 31, kZ = 1u << 30, kC = 1u << 29, kV = 1u << 28;

RC_INLINE bool CondHolds(u32 f, int c) {
    const bool n = f & kN, z = f & kZ, cf = f & kC, v = f & kV;
    bool r;
    switch (c >> 1) {
    case 0: r = z; break;
    case 1: r = cf; break;
    case 2: r = n; break;
    case 3: r = v; break;
    case 4: r = cf && !z; break;
    case 5: r = n == v; break;
    case 6: r = !z && n == v; break;
    default: return true; // AL / NV
    }
    return (c & 1) ? !r : r;
}

template <typename T>
struct FlagResult {
    T r;
    bool c, v;
    u32 nzcv;
};
template <typename T>
RC_INLINE FlagResult<T> AddWithCarry(T a, T b, bool cin) {
    constexpr int kBits = sizeof(T) * 8;
    const T r = T(a + b + T(cin));
    const bool c = cin ? (r <= a) : (r < a);
    const bool v = ((~(a ^ b) & (a ^ r)) >> (kBits - 1)) & 1;
    u32 f = (u32((r >> (kBits - 1)) & 1) << 31) | (r == 0 ? kZ : 0) | (c ? kC : 0) | (v ? kV : 0);
    return {r, c, v, f};
}
template <typename T>
RC_INLINE u32 NZOf(T r) {
    return (u32((r >> (sizeof(T) * 8 - 1)) & 1) << 31) | (r == 0 ? kZ : 0);
}

namespace op {

RC_INLINE u64 Pack2x32To1x64(u32 lo, u32 hi) { return u64(lo) | (u64(hi) << 32); }
RC_INLINE V128 Pack2x64To1x128(u64 lo, u64 hi) { return {lo, hi}; }
RC_INLINE u32 LeastSignificantWord(u64 a) { return u32(a); }
RC_INLINE u16 LeastSignificantHalf(u32 a) { return u16(a); }
RC_INLINE u8 LeastSignificantByte(u32 a) { return u8(a); }
RC_INLINE u32 MostSignificantWord(u64 a) { return u32(a >> 32); }
RC_INLINE bool MostSignificantBit(u32 a) { return a >> 31; }
RC_INLINE bool IsZero32(u32 a) { return a == 0; }
RC_INLINE bool IsZero64(u64 a) { return a == 0; }
RC_INLINE bool TestBit(u64 a, u8 b) { return (a >> (b & 63)) & 1; }
RC_INLINE u32 ConditionalSelect32(u32 f, int c, u32 a, u32 b) { return CondHolds(f, c) ? a : b; }
RC_INLINE u64 ConditionalSelect64(u32 f, int c, u64 a, u64 b) { return CondHolds(f, c) ? a : b; }
RC_INLINE u32 ConditionalSelectNZCV(u32 f, int c, u32 a, u32 b) { return CondHolds(f, c) ? a : b; }
RC_INLINE u32 NZCVFromPackedFlags(u32 a) { return a & 0xF0000000u; }
RC_INLINE bool GetCFlagFromNZCV(u32 a) { return (a & kC) != 0; }

RC_INLINE u32 LogicalShiftLeft32(u32 a, u8 s, bool) { return s >= 32 ? 0 : a << s; }
RC_INLINE u64 LogicalShiftLeft64(u64 a, u8 s) { return s >= 64 ? 0 : a << s; }
RC_INLINE u32 LogicalShiftRight32(u32 a, u8 s, bool) { return s >= 32 ? 0 : a >> s; }
RC_INLINE u64 LogicalShiftRight64(u64 a, u8 s) { return s >= 64 ? 0 : a >> s; }
RC_INLINE u32 ArithmeticShiftRight32(u32 a, u8 s, bool) { return u32(s32(a) >> (s >= 31 ? 31 : s)); }
RC_INLINE u64 ArithmeticShiftRight64(u64 a, u8 s) { return u64(s64(a) >> (s >= 63 ? 63 : s)); }
RC_INLINE u32 RotateRight32(u32 a, u8 s, bool) { return std::rotr(a, s & 31); }
RC_INLINE u64 RotateRight64(u64 a, u8 s) { return std::rotr(a, s & 63); }
RC_INLINE u32 LogicalShiftLeftMasked32(u32 a, u32 s) { return a << (s & 31); }
RC_INLINE u64 LogicalShiftLeftMasked64(u64 a, u64 s) { return a << (s & 63); }
RC_INLINE u32 LogicalShiftRightMasked32(u32 a, u32 s) { return a >> (s & 31); }
RC_INLINE u64 LogicalShiftRightMasked64(u64 a, u64 s) { return a >> (s & 63); }
RC_INLINE u32 ArithmeticShiftRightMasked32(u32 a, u32 s) { return u32(s32(a) >> (s & 31)); }
RC_INLINE u64 ArithmeticShiftRightMasked64(u64 a, u64 s) { return u64(s64(a) >> (s & 63)); }
RC_INLINE u32 RotateRightMasked32(u32 a, u32 s) { return std::rotr(a, int(s & 31)); }
RC_INLINE u64 RotateRightMasked64(u64 a, u64 s) { return std::rotr(a, int(s & 63)); }

RC_INLINE u32 Add32(u32 a, u32 b, bool c) { return a + b + u32(c); }
RC_INLINE u64 Add64(u64 a, u64 b, bool c) { return a + b + u64(c); }
RC_INLINE u32 Sub32(u32 a, u32 b, bool c) { return a + ~b + u32(c); }
RC_INLINE u64 Sub64(u64 a, u64 b, bool c) { return a + ~b + u64(c); }
RC_INLINE FlagResult<u32> Add32F(u32 a, u32 b, bool c) { return AddWithCarry<u32>(a, b, c); }
RC_INLINE FlagResult<u64> Add64F(u64 a, u64 b, bool c) { return AddWithCarry<u64>(a, b, c); }
RC_INLINE FlagResult<u32> Sub32F(u32 a, u32 b, bool c) { return AddWithCarry<u32>(a, ~b, c); }
RC_INLINE FlagResult<u64> Sub64F(u64 a, u64 b, bool c) { return AddWithCarry<u64>(a, ~b, c); }

RC_INLINE u32 Mul32(u32 a, u32 b) { return a * b; }
RC_INLINE u64 Mul64(u64 a, u64 b) { return a * b; }
RC_INLINE u64 SignedMultiplyHigh64(u64 a, u64 b) { return u64(__mulh(s64(a), s64(b))); }
RC_INLINE u64 UnsignedMultiplyHigh64(u64 a, u64 b) { return __umulh(a, b); }
RC_INLINE u32 UnsignedDiv32(u32 a, u32 b) { return b ? a / b : 0; }
RC_INLINE u64 UnsignedDiv64(u64 a, u64 b) { return b ? a / b : 0; }
RC_INLINE u32 SignedDiv32(u32 a, u32 b) {
    if (b == 0)
        return 0;
    if (a == 0x80000000u && b == 0xFFFFFFFFu)
        return a;
    return u32(s32(a) / s32(b));
}
RC_INLINE u64 SignedDiv64(u64 a, u64 b) {
    if (b == 0)
        return 0;
    if (a == 0x8000000000000000ull && b == ~0ull)
        return a;
    return u64(s64(a) / s64(b));
}
RC_INLINE u32 And32(u32 a, u32 b) { return a & b; }
RC_INLINE u64 And64(u64 a, u64 b) { return a & b; }
RC_INLINE u32 AndNot32(u32 a, u32 b) { return a & ~b; }
RC_INLINE u64 AndNot64(u64 a, u64 b) { return a & ~b; }
RC_INLINE u32 Eor32(u32 a, u32 b) { return a ^ b; }
RC_INLINE u64 Eor64(u64 a, u64 b) { return a ^ b; }
RC_INLINE u32 Or32(u32 a, u32 b) { return a | b; }
RC_INLINE u64 Or64(u64 a, u64 b) { return a | b; }
RC_INLINE u32 Not32(u32 a) { return ~a; }
RC_INLINE u64 Not64(u64 a) { return ~a; }
RC_INLINE u32 SignExtendByteToWord(u8 a) { return u32(s32(int8_t(a))); }
RC_INLINE u32 SignExtendHalfToWord(u16 a) { return u32(s32(int16_t(a))); }
RC_INLINE u64 SignExtendByteToLong(u8 a) { return u64(s64(int8_t(a))); }
RC_INLINE u64 SignExtendHalfToLong(u16 a) { return u64(s64(int16_t(a))); }
RC_INLINE u64 SignExtendWordToLong(u32 a) { return u64(s64(s32(a))); }
RC_INLINE u32 ZeroExtendByteToWord(u8 a) { return a; }
RC_INLINE u32 ZeroExtendHalfToWord(u16 a) { return a; }
RC_INLINE u64 ZeroExtendByteToLong(u8 a) { return a; }
RC_INLINE u64 ZeroExtendHalfToLong(u16 a) { return a; }
RC_INLINE u64 ZeroExtendWordToLong(u32 a) { return a; }
RC_INLINE V128 ZeroExtendLongToQuad(u64 a) { return {a, 0}; }
RC_INLINE u32 ByteReverseWord(u32 a) { return _byteswap_ulong(a); }
RC_INLINE u16 ByteReverseHalf(u16 a) { return _byteswap_ushort(a); }
RC_INLINE u64 ByteReverseDual(u64 a) { return _byteswap_uint64(a); }
RC_INLINE u32 CountLeadingZeros32(u32 a) { return u32(std::countl_zero(a)); }
RC_INLINE u64 CountLeadingZeros64(u64 a) { return u64(std::countl_zero(a)); }
RC_INLINE u32 ExtractRegister32(u32 a, u32 b, u8 lsb) { return lsb ? (a >> lsb) | (b << (32 - lsb)) : a; }
RC_INLINE u64 ExtractRegister64(u64 a, u64 b, u8 lsb) { return lsb ? (a >> lsb) | (b << (64 - lsb)) : a; }
RC_INLINE u32 ReplicateBit32(u32 a, u8 b) { return ((a >> b) & 1) ? ~0u : 0u; }
RC_INLINE u64 ReplicateBit64(u64 a, u8 b) { return ((a >> b) & 1) ? ~0ull : 0ull; }
RC_INLINE u32 MaxSigned32(u32 a, u32 b) { return s32(a) > s32(b) ? a : b; }
RC_INLINE u64 MaxSigned64(u64 a, u64 b) { return s64(a) > s64(b) ? a : b; }
RC_INLINE u32 MaxUnsigned32(u32 a, u32 b) { return a > b ? a : b; }
RC_INLINE u64 MaxUnsigned64(u64 a, u64 b) { return a > b ? a : b; }
RC_INLINE u32 MinSigned32(u32 a, u32 b) { return s32(a) < s32(b) ? a : b; }
RC_INLINE u64 MinSigned64(u64 a, u64 b) { return s64(a) < s64(b) ? a : b; }
RC_INLINE u32 MinUnsigned32(u32 a, u32 b) { return a < b ? a : b; }
RC_INLINE u64 MinUnsigned64(u64 a, u64 b) { return a < b ? a : b; }

RC_INLINE u32 CRC32Castagnoli8(u32 c, u32 v) { return _mm_crc32_u8(c, u8(v)); }
RC_INLINE u32 CRC32Castagnoli16(u32 c, u32 v) { return _mm_crc32_u16(c, u16(v)); }
RC_INLINE u32 CRC32Castagnoli32(u32 c, u32 v) { return _mm_crc32_u32(c, v); }
RC_INLINE u32 CRC32Castagnoli64(u32 c, u64 v) { return u32(_mm_crc32_u64(c, v)); }
RC_INLINE u32 Crc32Iso(u32 c, u64 v, int bytes) {
    for (int i = 0; i < bytes * 8; i++) {
        const u32 bit = (c ^ u32(v >> i)) & 1;
        c = (c >> 1) ^ (bit ? 0xEDB88320u : 0u);
    }
    return c;
}
RC_INLINE u32 CRC32ISO8(u32 c, u32 v) { return Crc32Iso(c, v, 1); }
RC_INLINE u32 CRC32ISO16(u32 c, u32 v) { return Crc32Iso(c, v, 2); }
RC_INLINE u32 CRC32ISO32(u32 c, u32 v) { return Crc32Iso(c, v, 4); }
RC_INLINE u32 CRC32ISO64(u32 c, u64 v) { return Crc32Iso(c, v, 8); }

// ---- 128-bit moves and element access
template <typename T>
RC_INLINE T GetElem(const V128& v, u8 i) {
    T r;
    std::memcpy(&r, reinterpret_cast<const u8*>(&v) + i * sizeof(T), sizeof(T));
    return r;
}
template <typename T>
RC_INLINE V128 SetElem(V128 v, u8 i, T x) {
    std::memcpy(reinterpret_cast<u8*>(&v) + i * sizeof(T), &x, sizeof(T));
    return v;
}
RC_INLINE u8 VectorGetElement8(V128 v, u8 i) { return GetElem<u8>(v, i); }
RC_INLINE u16 VectorGetElement16(V128 v, u8 i) { return GetElem<u16>(v, i); }
RC_INLINE u32 VectorGetElement32(V128 v, u8 i) { return GetElem<u32>(v, i); }
RC_INLINE u64 VectorGetElement64(V128 v, u8 i) { return GetElem<u64>(v, i); }
RC_INLINE V128 VectorSetElement8(V128 v, u8 i, u8 x) { return SetElem(v, i, x); }
RC_INLINE V128 VectorSetElement16(V128 v, u8 i, u16 x) { return SetElem(v, i, x); }
RC_INLINE V128 VectorSetElement32(V128 v, u8 i, u32 x) { return SetElem(v, i, x); }
RC_INLINE V128 VectorSetElement64(V128 v, u8 i, u64 x) { return SetElem(v, i, x); }
RC_INLINE V128 VectorZeroUpper(V128 v) { return {v.lo, 0}; }
RC_INLINE V128 ZeroVector() { return {0, 0}; }
RC_INLINE V128 VectorAnd(V128 a, V128 b) { return {a.lo & b.lo, a.hi & b.hi}; }
RC_INLINE V128 VectorAndNot(V128 a, V128 b) { return {a.lo & ~b.lo, a.hi & ~b.hi}; }
RC_INLINE V128 VectorOr(V128 a, V128 b) { return {a.lo | b.lo, a.hi | b.hi}; }
RC_INLINE V128 VectorEor(V128 a, V128 b) { return {a.lo ^ b.lo, a.hi ^ b.hi}; }
RC_INLINE V128 VectorNot(V128 a) { return {~a.lo, ~a.hi}; }
RC_INLINE V128 VectorBroadcast8(u8 a) { const u64 x = 0x0101010101010101ull * a; return {x, x}; }
RC_INLINE V128 VectorBroadcast16(u16 a) { const u64 x = 0x0001000100010001ull * a; return {x, x}; }
RC_INLINE V128 VectorBroadcast32(u32 a) { const u64 x = 0x0000000100000001ull * a; return {x, x}; }
RC_INLINE V128 VectorBroadcast64(u64 a) { return {a, a}; }
RC_INLINE V128 VectorBroadcastLower8(u8 a) { return {0x0101010101010101ull * a, 0}; }
RC_INLINE V128 VectorBroadcastLower16(u16 a) { return {0x0001000100010001ull * a, 0}; }
RC_INLINE V128 VectorBroadcastLower32(u32 a) { return {0x0000000100000001ull * a, 0}; }

// ---- scalar floating point on the host FPU (MXCSR mirrors FPCR rounding/flush-to-zero while native code runs)
RC_INLINE float F(u32 a) { return std::bit_cast<float>(a); }
RC_INLINE double D(u64 a) { return std::bit_cast<double>(a); }
RC_INLINE u32 B(float a) { return std::bit_cast<u32>(a); }
RC_INLINE u64 B(double a) { return std::bit_cast<u64>(a); }
RC_INLINE u32 FPAbs32(u32 a) { return a & 0x7FFFFFFFu; }
RC_INLINE u64 FPAbs64(u64 a) { return a & 0x7FFFFFFFFFFFFFFFull; }
RC_INLINE u32 FPNeg32(u32 a) { return a ^ 0x80000000u; }
RC_INLINE u64 FPNeg64(u64 a) { return a ^ 0x8000000000000000ull; }
RC_INLINE u32 FPAdd32(u32 a, u32 b) { return B(F(a) + F(b)); }
RC_INLINE u64 FPAdd64(u64 a, u64 b) { return B(D(a) + D(b)); }
RC_INLINE u32 FPSub32(u32 a, u32 b) { return B(F(a) - F(b)); }
RC_INLINE u64 FPSub64(u64 a, u64 b) { return B(D(a) - D(b)); }
RC_INLINE u32 FPMul32(u32 a, u32 b) { return B(F(a) * F(b)); }
RC_INLINE u64 FPMul64(u64 a, u64 b) { return B(D(a) * D(b)); }
RC_INLINE u32 FPDiv32(u32 a, u32 b) { return B(F(a) / F(b)); }
RC_INLINE u64 FPDiv64(u64 a, u64 b) { return B(D(a) / D(b)); }
RC_INLINE u32 FPSqrt32(u32 a) { return B(_mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(F(a))))); }
RC_INLINE u64 FPSqrt64(u64 a) { return B(_mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(D(a))))); }
// FPMulAdd(addend, a, b) = addend + a * b, fused
RC_INLINE u32 FPMulAdd32(u32 c, u32 a, u32 b) {
    return B(_mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(F(a)), _mm_set_ss(F(b)), _mm_set_ss(F(c)))));
}
RC_INLINE u64 FPMulAdd64(u64 c, u64 a, u64 b) {
    return B(_mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(D(a)), _mm_set_sd(D(b)), _mm_set_sd(D(c)))));
}
RC_INLINE u32 FPMulSub32(u32 c, u32 a, u32 b) {
    return B(_mm_cvtss_f32(_mm_fnmadd_ss(_mm_set_ss(F(a)), _mm_set_ss(F(b)), _mm_set_ss(F(c)))));
}
RC_INLINE u64 FPMulSub64(u64 c, u64 a, u64 b) {
    return B(_mm_cvtsd_f64(_mm_fnmadd_sd(_mm_set_sd(D(a)), _mm_set_sd(D(b)), _mm_set_sd(D(c)))));
}
template <typename T>
RC_INLINE u32 FPCmp(T a, T b) {
    if (a != a || b != b)
        return kC | kV;
    if (a == b)
        return kZ | kC;
    return a < b ? kN : kC;
}
RC_INLINE u32 FPCompare32(u32 a, u32 b, bool) { return FPCmp(F(a), F(b)); }
RC_INLINE u32 FPCompare64(u64 a, u64 b, bool) { return FPCmp(D(a), D(b)); }
template <typename T, typename U>
RC_INLINE bool IsNaN(U bits) {
    const T x = std::bit_cast<T>(bits);
    return x != x;
}
template <typename U>
constexpr U kQuiet = sizeof(U) == 4 ? U(0x00400000u) : U(0x0008000000000000ull);
// ARM NaN propagation: first signalling NaN (quieted), else first quiet NaN.
template <typename T, typename U>
RC_INLINE U PropagateNaN(U a, U b) {
    auto snan = [](U x) { return IsNaN<T>(x) && !(x & kQuiet<U>); };
    if (snan(a))
        return a | kQuiet<U>;
    if (snan(b))
        return b | kQuiet<U>;
    return IsNaN<T>(a) ? a : b;
}
template <typename T, typename U, bool kMax>
RC_INLINE U FPMaxMin(U a, U b) {
    if (IsNaN<T>(a) || IsNaN<T>(b))
        return PropagateNaN<T>(a, b);
    const T x = std::bit_cast<T>(a), y = std::bit_cast<T>(b);
    if (x == y) // includes +0/-0: max prefers +0, min prefers -0
        return kMax ? (a & b) : (a | b);
    return kMax ? (x > y ? a : b) : (x < y ? a : b);
}
template <typename T, typename U, bool kMax>
RC_INLINE U FPMaxMinNumeric(U a, U b) {
    const bool qa = IsNaN<T>(a) && (a & kQuiet<U>), qb = IsNaN<T>(b) && (b & kQuiet<U>);
    if (qa && !IsNaN<T>(b))
        return b;
    if (qb && !IsNaN<T>(a))
        return a;
    return FPMaxMin<T, U, kMax>(a, b);
}
RC_INLINE u32 FPMax32(u32 a, u32 b) { return FPMaxMin<float, u32, true>(a, b); }
RC_INLINE u64 FPMax64(u64 a, u64 b) { return FPMaxMin<double, u64, true>(a, b); }
RC_INLINE u32 FPMin32(u32 a, u32 b) { return FPMaxMin<float, u32, false>(a, b); }
RC_INLINE u64 FPMin64(u64 a, u64 b) { return FPMaxMin<double, u64, false>(a, b); }
RC_INLINE u32 FPMaxNumeric32(u32 a, u32 b) { return FPMaxMinNumeric<float, u32, true>(a, b); }
RC_INLINE u64 FPMaxNumeric64(u64 a, u64 b) { return FPMaxMinNumeric<double, u64, true>(a, b); }
RC_INLINE u32 FPMinNumeric32(u32 a, u32 b) { return FPMaxMinNumeric<float, u32, false>(a, b); }
RC_INLINE u64 FPMinNumeric64(u64 a, u64 b) { return FPMaxMinNumeric<double, u64, false>(a, b); }
RC_INLINE u32 FPMulX32(u32 a, u32 b) {
    const float x = F(a), y = F(b);
    if ((std::isinf(x) && y == 0) || (x == 0 && std::isinf(y)))
        return ((a ^ b) & 0x80000000u) | 0x40000000u;
    return B(x * y);
}
RC_INLINE u64 FPMulX64(u64 a, u64 b) {
    const double x = D(a), y = D(b);
    if ((std::isinf(x) && y == 0) || (x == 0 && std::isinf(y)))
        return ((a ^ b) & 0x8000000000000000ull) | 0x4000000000000000ull;
    return B(x * y);
}
RC_INLINE u64 FPSingleToDouble(u32 a, u8) {
    if (IsNaN<float>(a)) // keep the payload, quiet it
        return (u64(a & 0x80000000u) << 32) | 0x7FF8000000000000ull | (u64(a & 0x003FFFFFu) << 29);
    return B(double(F(a)));
}
RC_INLINE u32 FPDoubleToSingle(u64 a, u8) {
    if (IsNaN<double>(a))
        return u32((a >> 32) & 0x80000000u) | 0x7FC00000u | u32((a >> 29) & 0x003FFFFFu);
    return B(float(D(a)));
}
template <typename To, typename From>
RC_INLINE To FixedToFP(From v, u8 fbits) {
    To r = To(v);
    if (fbits)
        r = r / To(u64(1) << fbits);
    return r;
}
RC_INLINE u32 FPFixedU16ToSingle(u16 a, u8 f, u8) { return B(FixedToFP<float>(a, f)); }
RC_INLINE u32 FPFixedS16ToSingle(u16 a, u8 f, u8) { return B(FixedToFP<float>(int16_t(a), f)); }
RC_INLINE u64 FPFixedU16ToDouble(u16 a, u8 f, u8) { return B(FixedToFP<double>(a, f)); }
RC_INLINE u64 FPFixedS16ToDouble(u16 a, u8 f, u8) { return B(FixedToFP<double>(int16_t(a), f)); }
RC_INLINE u32 FPFixedU32ToSingle(u32 a, u8 f, u8) { return B(FixedToFP<float>(a, f)); }
RC_INLINE u32 FPFixedS32ToSingle(u32 a, u8 f, u8) { return B(FixedToFP<float>(s32(a), f)); }
RC_INLINE u64 FPFixedU32ToDouble(u32 a, u8 f, u8) { return B(FixedToFP<double>(a, f)); }
RC_INLINE u64 FPFixedS32ToDouble(u32 a, u8 f, u8) { return B(FixedToFP<double>(s32(a), f)); }
RC_INLINE u64 FPFixedU64ToDouble(u64 a, u8 f, u8) { return B(FixedToFP<double>(a, f)); }
RC_INLINE u32 FPFixedU64ToSingle(u64 a, u8 f, u8) { return B(FixedToFP<float>(a, f)); }
RC_INLINE u64 FPFixedS64ToDouble(u64 a, u8 f, u8) { return B(FixedToFP<double>(s64(a), f)); }
RC_INLINE u32 FPFixedS64ToSingle(u64 a, u8 f, u8) { return B(FixedToFP<float>(s64(a), f)); }

// ---- SIMD (lane order: element 0 = least significant)
RC_INLINE __m128 PS(V128 v) { return _mm_castsi128_ps(_mm_set_epi64x(s64(v.hi), s64(v.lo))); }
RC_INLINE __m128d PD(V128 v) { return _mm_castsi128_pd(_mm_set_epi64x(s64(v.hi), s64(v.lo))); }
RC_INLINE __m128i PI(V128 v) { return _mm_set_epi64x(s64(v.hi), s64(v.lo)); }
RC_INLINE V128 VI(__m128i x) {
    V128 r;
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&r), x);
    return r;
}
RC_INLINE V128 VS(__m128 x) { return VI(_mm_castps_si128(x)); }
RC_INLINE V128 VD(__m128d x) { return VI(_mm_castpd_si128(x)); }
template <typename T, typename F>
RC_INLINE V128 Map1(V128 a, F f) {
    V128 r;
    for (u8 i = 0; i < 16 / sizeof(T); i++)
        r = SetElem<T>(i ? r : V128{}, i, f(GetElem<T>(a, i)));
    return r;
}
template <typename T, typename F>
RC_INLINE V128 Map2(V128 a, V128 b, F f) {
    V128 r{};
    for (u8 i = 0; i < 16 / sizeof(T); i++)
        r = SetElem<T>(r, i, f(GetElem<T>(a, i), GetElem<T>(b, i)));
    return r;
}

RC_INLINE V128 FPVectorAdd32(V128 a, V128 b, bool) { return VS(_mm_add_ps(PS(a), PS(b))); }
RC_INLINE V128 FPVectorAdd64(V128 a, V128 b, bool) { return VD(_mm_add_pd(PD(a), PD(b))); }
RC_INLINE V128 FPVectorSub32(V128 a, V128 b, bool) { return VS(_mm_sub_ps(PS(a), PS(b))); }
RC_INLINE V128 FPVectorSub64(V128 a, V128 b, bool) { return VD(_mm_sub_pd(PD(a), PD(b))); }
RC_INLINE V128 FPVectorMul32(V128 a, V128 b, bool) { return VS(_mm_mul_ps(PS(a), PS(b))); }
RC_INLINE V128 FPVectorMul64(V128 a, V128 b, bool) { return VD(_mm_mul_pd(PD(a), PD(b))); }
RC_INLINE V128 FPVectorDiv32(V128 a, V128 b, bool) { return VS(_mm_div_ps(PS(a), PS(b))); }
RC_INLINE V128 FPVectorDiv64(V128 a, V128 b, bool) { return VD(_mm_div_pd(PD(a), PD(b))); }
RC_INLINE V128 FPVectorSqrt32(V128 a, bool) { return VS(_mm_sqrt_ps(PS(a))); }
RC_INLINE V128 FPVectorSqrt64(V128 a, bool) { return VD(_mm_sqrt_pd(PD(a))); }
RC_INLINE V128 FPVectorMulAdd32(V128 c, V128 a, V128 b, bool) { return VS(_mm_fmadd_ps(PS(a), PS(b), PS(c))); }
RC_INLINE V128 FPVectorMulAdd64(V128 c, V128 a, V128 b, bool) { return VD(_mm_fmadd_pd(PD(a), PD(b), PD(c))); }
RC_INLINE V128 FPVectorNeg32(V128 a) { return {a.lo ^ 0x8000000080000000ull, a.hi ^ 0x8000000080000000ull}; }
RC_INLINE V128 FPVectorNeg64(V128 a) { return {a.lo ^ 0x8000000000000000ull, a.hi ^ 0x8000000000000000ull}; }
RC_INLINE V128 FPVectorAbs32(V128 a) { return {a.lo & 0x7FFFFFFF7FFFFFFFull, a.hi & 0x7FFFFFFF7FFFFFFFull}; }
RC_INLINE V128 FPVectorAbs64(V128 a) { return {a.lo & 0x7FFFFFFFFFFFFFFFull, a.hi & 0x7FFFFFFFFFFFFFFFull}; }
RC_INLINE V128 FPVectorMax32(V128 a, V128 b, bool) { return Map2<u32>(a, b, FPMax32); }
RC_INLINE V128 FPVectorMax64(V128 a, V128 b, bool) { return Map2<u64>(a, b, FPMax64); }
RC_INLINE V128 FPVectorMin32(V128 a, V128 b, bool) { return Map2<u32>(a, b, FPMin32); }
RC_INLINE V128 FPVectorMin64(V128 a, V128 b, bool) { return Map2<u64>(a, b, FPMin64); }
RC_INLINE V128 FPVectorMaxNumeric32(V128 a, V128 b, bool) { return Map2<u32>(a, b, FPMaxNumeric32); }
RC_INLINE V128 FPVectorMaxNumeric64(V128 a, V128 b, bool) { return Map2<u64>(a, b, FPMaxNumeric64); }
RC_INLINE V128 FPVectorMinNumeric32(V128 a, V128 b, bool) { return Map2<u32>(a, b, FPMinNumeric32); }
RC_INLINE V128 FPVectorMinNumeric64(V128 a, V128 b, bool) { return Map2<u64>(a, b, FPMinNumeric64); }
RC_INLINE V128 FPVectorMulX32(V128 a, V128 b, bool) { return Map2<u32>(a, b, FPMulX32); }
RC_INLINE V128 FPVectorMulX64(V128 a, V128 b, bool) { return Map2<u64>(a, b, FPMulX64); }
RC_INLINE V128 FPVectorEqual32(V128 a, V128 b, bool) { return VS(_mm_cmpeq_ps(PS(a), PS(b))); }
RC_INLINE V128 FPVectorEqual64(V128 a, V128 b, bool) { return VD(_mm_cmpeq_pd(PD(a), PD(b))); }
RC_INLINE V128 FPVectorGreater32(V128 a, V128 b, bool) { return VS(_mm_cmpgt_ps(PS(a), PS(b))); }
RC_INLINE V128 FPVectorGreater64(V128 a, V128 b, bool) { return VD(_mm_cmpgt_pd(PD(a), PD(b))); }
RC_INLINE V128 FPVectorGreaterEqual32(V128 a, V128 b, bool) { return VS(_mm_cmpge_ps(PS(a), PS(b))); }
RC_INLINE V128 FPVectorGreaterEqual64(V128 a, V128 b, bool) { return VD(_mm_cmpge_pd(PD(a), PD(b))); }
RC_INLINE V128 FPVectorPairedAdd32(V128 a, V128 b, bool) { return VS(_mm_hadd_ps(PS(a), PS(b))); }
RC_INLINE V128 FPVectorPairedAdd64(V128 a, V128 b, bool) { return VD(_mm_hadd_pd(PD(a), PD(b))); }
RC_INLINE V128 FPVectorPairedAddLower32(V128 a, V128 b, bool) {
    return {u64(FPAdd32(u32(a.lo), u32(a.lo >> 32))) | (u64(FPAdd32(u32(b.lo), u32(b.lo >> 32))) << 32), 0};
}
RC_INLINE V128 FPVectorPairedAddLower64(V128 a, V128 b, bool) { return {FPAdd64(a.lo, b.lo), 0}; }
RC_INLINE V128 FPVectorFromSignedFixed32(V128 a, u8 f, u8, bool) {
    return Map1<u32>(a, [f](u32 x) { return B(FixedToFP<float>(s32(x), f)); });
}
RC_INLINE V128 FPVectorFromUnsignedFixed32(V128 a, u8 f, u8, bool) {
    return Map1<u32>(a, [f](u32 x) { return B(FixedToFP<float>(x, f)); });
}
RC_INLINE V128 FPVectorFromSignedFixed64(V128 a, u8 f, u8, bool) {
    return Map1<u64>(a, [f](u64 x) { return B(FixedToFP<double>(s64(x), f)); });
}
RC_INLINE V128 FPVectorFromUnsignedFixed64(V128 a, u8 f, u8, bool) {
    return Map1<u64>(a, [f](u64 x) { return B(FixedToFP<double>(x, f)); });
}

template <typename T>
RC_INLINE V128 Bcast(T x) {
    V128 r{};
    for (u8 i = 0; i < 16 / sizeof(T); i++)
        r = SetElem<T>(r, i, x);
    return r;
}
RC_INLINE V128 VectorBroadcastElement8(V128 v, u8 i) { return Bcast(GetElem<u8>(v, i)); }
RC_INLINE V128 VectorBroadcastElement16(V128 v, u8 i) { return Bcast(GetElem<u16>(v, i)); }
RC_INLINE V128 VectorBroadcastElement32(V128 v, u8 i) { return Bcast(GetElem<u32>(v, i)); }
RC_INLINE V128 VectorBroadcastElement64(V128 v, u8 i) { return Bcast(GetElem<u64>(v, i)); }
RC_INLINE V128 VectorBroadcastElementLower8(V128 v, u8 i) { return {Bcast(GetElem<u8>(v, i)).lo, 0}; }
RC_INLINE V128 VectorBroadcastElementLower16(V128 v, u8 i) { return {Bcast(GetElem<u16>(v, i)).lo, 0}; }
RC_INLINE V128 VectorBroadcastElementLower32(V128 v, u8 i) { return {Bcast(GetElem<u32>(v, i)).lo, 0}; }

RC_INLINE V128 VectorAdd8(V128 a, V128 b) { return VI(_mm_add_epi8(PI(a), PI(b))); }
RC_INLINE V128 VectorAdd16(V128 a, V128 b) { return VI(_mm_add_epi16(PI(a), PI(b))); }
RC_INLINE V128 VectorAdd32(V128 a, V128 b) { return VI(_mm_add_epi32(PI(a), PI(b))); }
RC_INLINE V128 VectorAdd64(V128 a, V128 b) { return {a.lo + b.lo, a.hi + b.hi}; }
RC_INLINE V128 VectorSub8(V128 a, V128 b) { return VI(_mm_sub_epi8(PI(a), PI(b))); }
RC_INLINE V128 VectorSub16(V128 a, V128 b) { return VI(_mm_sub_epi16(PI(a), PI(b))); }
RC_INLINE V128 VectorSub32(V128 a, V128 b) { return VI(_mm_sub_epi32(PI(a), PI(b))); }
RC_INLINE V128 VectorSub64(V128 a, V128 b) { return {a.lo - b.lo, a.hi - b.hi}; }
RC_INLINE V128 VectorMultiply16(V128 a, V128 b) { return VI(_mm_mullo_epi16(PI(a), PI(b))); }
RC_INLINE V128 VectorMultiply32(V128 a, V128 b) { return VI(_mm_mullo_epi32(PI(a), PI(b))); }
RC_INLINE V128 VectorEqual8(V128 a, V128 b) { return VI(_mm_cmpeq_epi8(PI(a), PI(b))); }
RC_INLINE V128 VectorEqual16(V128 a, V128 b) { return VI(_mm_cmpeq_epi16(PI(a), PI(b))); }
RC_INLINE V128 VectorEqual32(V128 a, V128 b) { return VI(_mm_cmpeq_epi32(PI(a), PI(b))); }
RC_INLINE V128 VectorEqual64(V128 a, V128 b) { return {a.lo == b.lo ? ~0ull : 0, a.hi == b.hi ? ~0ull : 0}; }
RC_INLINE V128 VectorEqual128(V128 a, V128 b) {
    const u64 m = (a.lo == b.lo && a.hi == b.hi) ? ~0ull : 0;
    return {m, m};
}
RC_INLINE V128 VectorGreaterS8(V128 a, V128 b) { return VI(_mm_cmpgt_epi8(PI(a), PI(b))); }
RC_INLINE V128 VectorGreaterS16(V128 a, V128 b) { return VI(_mm_cmpgt_epi16(PI(a), PI(b))); }
RC_INLINE V128 VectorGreaterS32(V128 a, V128 b) { return VI(_mm_cmpgt_epi32(PI(a), PI(b))); }
RC_INLINE V128 VectorGreaterS64(V128 a, V128 b) { return VI(_mm_cmpgt_epi64(PI(a), PI(b))); }
RC_INLINE V128 VectorMinU8(V128 a, V128 b) { return VI(_mm_min_epu8(PI(a), PI(b))); }
RC_INLINE V128 VectorMinU16(V128 a, V128 b) { return VI(_mm_min_epu16(PI(a), PI(b))); }
RC_INLINE V128 VectorMinU32(V128 a, V128 b) { return VI(_mm_min_epu32(PI(a), PI(b))); }
RC_INLINE V128 VectorMaxU8(V128 a, V128 b) { return VI(_mm_max_epu8(PI(a), PI(b))); }
RC_INLINE V128 VectorMaxU16(V128 a, V128 b) { return VI(_mm_max_epu16(PI(a), PI(b))); }
RC_INLINE V128 VectorMaxU32(V128 a, V128 b) { return VI(_mm_max_epu32(PI(a), PI(b))); }
RC_INLINE V128 VectorMinS8(V128 a, V128 b) { return VI(_mm_min_epi8(PI(a), PI(b))); }
RC_INLINE V128 VectorMinS16(V128 a, V128 b) { return VI(_mm_min_epi16(PI(a), PI(b))); }
RC_INLINE V128 VectorMinS32(V128 a, V128 b) { return VI(_mm_min_epi32(PI(a), PI(b))); }
RC_INLINE V128 VectorMaxS8(V128 a, V128 b) { return VI(_mm_max_epi8(PI(a), PI(b))); }
RC_INLINE V128 VectorMaxS16(V128 a, V128 b) { return VI(_mm_max_epi16(PI(a), PI(b))); }
RC_INLINE V128 VectorMaxS32(V128 a, V128 b) { return VI(_mm_max_epi32(PI(a), PI(b))); }

template <typename T>
RC_INLINE V128 Shl(V128 a, u8 s) {
    return Map1<T>(a, [s](T x) { return s >= sizeof(T) * 8 ? T(0) : T(x << s); });
}
template <typename T>
RC_INLINE V128 Shr(V128 a, u8 s) {
    return Map1<T>(a, [s](T x) { return s >= sizeof(T) * 8 ? T(0) : T(x >> s); });
}
template <typename T>
RC_INLINE V128 Sar(V128 a, u8 s) {
    using S = std::make_signed_t<T>;
    return Map1<T>(a, [s](T x) { return T(S(x) >> (s >= sizeof(T) * 8 ? sizeof(T) * 8 - 1 : s)); });
}
RC_INLINE V128 VectorLogicalShiftLeft8(V128 a, u8 s) { return Shl<u8>(a, s); }
RC_INLINE V128 VectorLogicalShiftLeft16(V128 a, u8 s) { return Shl<u16>(a, s); }
RC_INLINE V128 VectorLogicalShiftLeft32(V128 a, u8 s) { return Shl<u32>(a, s); }
RC_INLINE V128 VectorLogicalShiftLeft64(V128 a, u8 s) { return Shl<u64>(a, s); }
RC_INLINE V128 VectorLogicalShiftRight8(V128 a, u8 s) { return Shr<u8>(a, s); }
RC_INLINE V128 VectorLogicalShiftRight16(V128 a, u8 s) { return Shr<u16>(a, s); }
RC_INLINE V128 VectorLogicalShiftRight32(V128 a, u8 s) { return Shr<u32>(a, s); }
RC_INLINE V128 VectorLogicalShiftRight64(V128 a, u8 s) { return Shr<u64>(a, s); }
RC_INLINE V128 VectorArithmeticShiftRight8(V128 a, u8 s) { return Sar<u8>(a, s); }
RC_INLINE V128 VectorArithmeticShiftRight16(V128 a, u8 s) { return Sar<u16>(a, s); }
RC_INLINE V128 VectorArithmeticShiftRight32(V128 a, u8 s) { return Sar<u32>(a, s); }
RC_INLINE V128 VectorArithmeticShiftRight64(V128 a, u8 s) { return Sar<u64>(a, s); }

// widen the lower half
template <typename From, typename To>
RC_INLINE V128 Widen(V128 a) {
    V128 r{};
    for (u8 i = 0; i < 16 / sizeof(To); i++)
        r = SetElem<To>(r, i, To(GetElem<From>(a, i)));
    return r;
}
RC_INLINE V128 VectorSignExtend8(V128 a) { return Widen<int8_t, int16_t>(a); }
RC_INLINE V128 VectorSignExtend16(V128 a) { return Widen<int16_t, int32_t>(a); }
RC_INLINE V128 VectorSignExtend32(V128 a) { return Widen<int32_t, int64_t>(a); }
RC_INLINE V128 VectorZeroExtend8(V128 a) { return Widen<u8, u16>(a); }
RC_INLINE V128 VectorZeroExtend16(V128 a) { return Widen<u16, u32>(a); }
RC_INLINE V128 VectorZeroExtend32(V128 a) { return Widen<u32, u64>(a); }
RC_INLINE V128 VectorZeroExtend64(V128 a) { return {a.lo, 0}; }
template <typename From, typename To>
RC_INLINE V128 Narrow(V128 a) {
    V128 r{};
    for (u8 i = 0; i < 16 / sizeof(From); i++)
        r = SetElem<To>(r, i, To(GetElem<From>(a, i)));
    return r;
}
RC_INLINE V128 VectorNarrow16(V128 a) { return Narrow<u16, u8>(a); }
RC_INLINE V128 VectorNarrow32(V128 a) { return Narrow<u32, u16>(a); }
RC_INLINE V128 VectorNarrow64(V128 a) { return Narrow<u64, u32>(a); }

template <typename T>
RC_INLINE V128 InterleaveLower(V128 a, V128 b) {
    constexpr u8 n = 16 / sizeof(T);
    V128 r{};
    for (u8 i = 0; i < n / 2; i++) {
        r = SetElem<T>(r, 2 * i, GetElem<T>(a, i));
        r = SetElem<T>(r, 2 * i + 1, GetElem<T>(b, i));
    }
    return r;
}
template <typename T>
RC_INLINE V128 InterleaveUpper(V128 a, V128 b) {
    constexpr u8 n = 16 / sizeof(T);
    V128 r{};
    for (u8 i = 0; i < n / 2; i++) {
        r = SetElem<T>(r, 2 * i, GetElem<T>(a, n / 2 + i));
        r = SetElem<T>(r, 2 * i + 1, GetElem<T>(b, n / 2 + i));
    }
    return r;
}
RC_INLINE V128 VectorInterleaveLower8(V128 a, V128 b) { return InterleaveLower<u8>(a, b); }
RC_INLINE V128 VectorInterleaveLower16(V128 a, V128 b) { return InterleaveLower<u16>(a, b); }
RC_INLINE V128 VectorInterleaveLower32(V128 a, V128 b) { return InterleaveLower<u32>(a, b); }
RC_INLINE V128 VectorInterleaveLower64(V128 a, V128 b) { return InterleaveLower<u64>(a, b); }
RC_INLINE V128 VectorInterleaveUpper8(V128 a, V128 b) { return InterleaveUpper<u8>(a, b); }
RC_INLINE V128 VectorInterleaveUpper16(V128 a, V128 b) { return InterleaveUpper<u16>(a, b); }
RC_INLINE V128 VectorInterleaveUpper32(V128 a, V128 b) { return InterleaveUpper<u32>(a, b); }
RC_INLINE V128 VectorInterleaveUpper64(V128 a, V128 b) { return InterleaveUpper<u64>(a, b); }
// EXT: (b:a) >> pos bits
RC_INLINE V128 VectorExtract(V128 a, V128 b, u8 pos) {
    u8 buf[32];
    std::memcpy(buf, &a, 16);
    std::memcpy(buf + 16, &b, 16);
    V128 r;
    std::memcpy(&r, buf + pos / 8, 16);
    return r;
}
RC_INLINE V128 VectorExtractLower(V128 a, V128 b, u8 pos) {
    u8 buf[16];
    std::memcpy(buf, &a.lo, 8);
    std::memcpy(buf + 8, &b.lo, 8);
    V128 r{};
    std::memcpy(&r.lo, buf + pos / 8, 8);
    return r;
}

// TBL/TBX: bytes indexing past the table take the default (zero for TBL, the destination for TBX)
struct Tbl {
    V128 t[4];
    int n;
};
RC_INLINE V128 VectorTableLookup128(V128 def, const Tbl& tb, V128 idx) {
    u8 table[64], d[16], ix[16], out[16];
    std::memcpy(table, tb.t, sizeof(V128) * tb.n);
    std::memcpy(d, &def, 16);
    std::memcpy(ix, &idx, 16);
    for (int i = 0; i < 16; i++)
        out[i] = ix[i] < 16 * tb.n ? table[ix[i]] : d[i];
    V128 r;
    std::memcpy(&r, out, 16);
    return r;
}
// USHL/SSHL by register: signed low byte of each lane of b, negative = right shift
template <typename T>
RC_INLINE V128 VShiftL(V128 a, V128 b) {
    return Map2<T>(a, b, [](T x, T y) {
        constexpr int kBits = sizeof(T) * 8;
        const int sh = int8_t(u8(y));
        if (sh >= 0)
            return sh >= kBits ? T(0) : T(x << sh);
        return -sh >= kBits ? T(0) : T(x >> -sh);
    });
}
template <typename T>
RC_INLINE V128 VShiftA(V128 a, V128 b) {
    using S = std::make_signed_t<T>;
    return Map2<T>(a, b, [](T x, T y) {
        constexpr int kBits = sizeof(T) * 8;
        const int sh = int8_t(u8(y));
        if (sh >= 0)
            return sh >= kBits ? T(0) : T(x << sh);
        return T(S(x) >> (-sh >= kBits ? kBits - 1 : -sh));
    });
}
RC_INLINE V128 VectorLogicalVShift8(V128 a, V128 b) { return VShiftL<u8>(a, b); }
RC_INLINE V128 VectorLogicalVShift16(V128 a, V128 b) { return VShiftL<u16>(a, b); }
RC_INLINE V128 VectorLogicalVShift32(V128 a, V128 b) { return VShiftL<u32>(a, b); }
RC_INLINE V128 VectorLogicalVShift64(V128 a, V128 b) { return VShiftL<u64>(a, b); }
RC_INLINE V128 VectorArithmeticVShift8(V128 a, V128 b) { return VShiftA<u8>(a, b); }
RC_INLINE V128 VectorArithmeticVShift16(V128 a, V128 b) { return VShiftA<u16>(a, b); }
RC_INLINE V128 VectorArithmeticVShift32(V128 a, V128 b) { return VShiftA<u32>(a, b); }
RC_INLINE V128 VectorArithmeticVShift64(V128 a, V128 b) { return VShiftA<u64>(a, b); }
RC_INLINE V128 VectorRotateWholeVectorRight(V128 a, u8 bits) {
    if (bits == 0)
        return a;
    if (bits == 64)
        return {a.hi, a.lo};
    if (bits < 64)
        return {(a.lo >> bits) | (a.hi << (64 - bits)), (a.hi >> bits) | (a.lo << (64 - bits))};
    const u8 b2 = bits - 64;
    return {(a.hi >> b2) | (a.lo << (64 - b2)), (a.lo >> b2) | (a.hi << (64 - b2))};
}

// ---- byte/element reversal, widening multiplies, reductions, transposes
template <typename G, typename E>
RC_INLINE V128 ReverseIn(V128 a) {
    constexpr int n = sizeof(G) / sizeof(E);
    V128 r{};
    for (u8 g = 0; g < 16 / sizeof(G); g++)
        for (u8 i = 0; i < n; i++)
            r = SetElem<E>(r, u8(g * n + i), GetElem<E>(a, u8(g * n + n - 1 - i)));
    return r;
}
RC_INLINE V128 VectorReverseElementsInHalfGroups8(V128 a) { return ReverseIn<u16, u8>(a); }
RC_INLINE V128 VectorReverseElementsInWordGroups8(V128 a) { return ReverseIn<u32, u8>(a); }
RC_INLINE V128 VectorReverseElementsInWordGroups16(V128 a) { return ReverseIn<u32, u16>(a); }
RC_INLINE V128 VectorReverseElementsInLongGroups8(V128 a) { return ReverseIn<u64, u8>(a); }
RC_INLINE V128 VectorReverseElementsInLongGroups16(V128 a) { return ReverseIn<u64, u16>(a); }
RC_INLINE V128 VectorReverseElementsInLongGroups32(V128 a) { return ReverseIn<u64, u32>(a); }
// SMULL on the lower halves
template <typename From, typename To>
RC_INLINE V128 MulWiden(V128 a, V128 b) {
    V128 r{};
    for (u8 i = 0; i < 16 / sizeof(To); i++)
        r = SetElem<To>(r, i, To(To(GetElem<From>(a, i)) * To(GetElem<From>(b, i))));
    return r;
}
RC_INLINE V128 VectorMultiplySignedWiden8(V128 a, V128 b) { return MulWiden<int8_t, int16_t>(a, b); }
RC_INLINE V128 VectorMultiplySignedWiden16(V128 a, V128 b) { return MulWiden<int16_t, int32_t>(a, b); }
RC_INLINE V128 VectorMultiplySignedWiden32(V128 a, V128 b) { return MulWiden<int32_t, int64_t>(a, b); }
RC_INLINE V128 VectorPolynomialMultiplyLong64(V128 a, V128 b) {
    return VI(_mm_clmulepi64_si128(PI(a), PI(b), 0x00));
}
RC_INLINE V128 VectorPopulationCount(V128 a) {
    return Map1<u8>(a, [](u8 x) { return u8(std::popcount(x)); });
}
template <typename T>
RC_INLINE V128 ReduceAdd(V128 a) {
    T sum = 0;
    for (u8 i = 0; i < 16 / sizeof(T); i++)
        sum = T(sum + GetElem<T>(a, i));
    return SetElem<T>(V128{}, 0, sum);
}
RC_INLINE V128 VectorReduceAdd8(V128 a) { return ReduceAdd<u8>(a); }
RC_INLINE V128 VectorReduceAdd16(V128 a) { return ReduceAdd<u16>(a); }
RC_INLINE V128 VectorReduceAdd32(V128 a) { return ReduceAdd<u32>(a); }
RC_INLINE V128 VectorReduceAdd64(V128 a) { return ReduceAdd<u64>(a); }
// TRN1 (part = false) / TRN2 (part = true)
template <typename T>
RC_INLINE V128 Transpose(V128 a, V128 b, bool part) {
    V128 r{};
    for (u8 i = 0; i < 16 / sizeof(T); i += 2) {
        r = SetElem<T>(r, i, GetElem<T>(a, u8(i + part)));
        r = SetElem<T>(r, u8(i + 1), GetElem<T>(b, u8(i + part)));
    }
    return r;
}
RC_INLINE V128 VectorTranspose8(V128 a, V128 b, bool p) { return Transpose<u8>(a, b, p); }
RC_INLINE V128 VectorTranspose16(V128 a, V128 b, bool p) { return Transpose<u16>(a, b, p); }
RC_INLINE V128 VectorTranspose32(V128 a, V128 b, bool p) { return Transpose<u32>(a, b, p); }
RC_INLINE V128 VectorTranspose64(V128 a, V128 b, bool p) { return Transpose<u64>(a, b, p); }

// ---- SHA256 (ARM pseudocode: SHA256hash / SHA256SU0 / SHA256SU1)
RC_INLINE u32 Ror32(u32 x, int n) { return std::rotr(x, n); }
RC_INLINE V128 SHA256Hash(V128 x, V128 y, V128 w, bool part1) {
    u32 X[4], Y[4], W[4];
    std::memcpy(X, &x, 16);
    std::memcpy(Y, &y, 16);
    std::memcpy(W, &w, 16);
    for (int e = 0; e < 4; e++) {
        const u32 chs = ((Y[1] ^ Y[2]) & Y[0]) ^ Y[2];
        const u32 maj = (X[0] & X[1]) | ((X[0] | X[1]) & X[2]);
        const u32 t = Y[3] + (Ror32(Y[0], 6) ^ Ror32(Y[0], 11) ^ Ror32(Y[0], 25)) + chs + W[e];
        X[3] += t;
        Y[3] = t + (Ror32(X[0], 2) ^ Ror32(X[0], 13) ^ Ror32(X[0], 22)) + maj;
        // (Y:X) rotated left by one word
        const u32 y3 = Y[3], x3 = X[3];
        Y[3] = Y[2], Y[2] = Y[1], Y[1] = Y[0], Y[0] = x3;
        X[3] = X[2], X[2] = X[1], X[1] = X[0], X[0] = y3;
    }
    V128 r;
    std::memcpy(&r, part1 ? X : Y, 16);
    return r;
}
RC_INLINE V128 SHA256MessageSchedule0(V128 x, V128 y) {
    u32 X[4], Y[4], T[4], R[4];
    std::memcpy(X, &x, 16);
    std::memcpy(Y, &y, 16);
    T[0] = X[1], T[1] = X[2], T[2] = X[3], T[3] = Y[0];
    for (int e = 0; e < 4; e++)
        R[e] = (Ror32(T[e], 7) ^ Ror32(T[e], 18) ^ (T[e] >> 3)) + X[e];
    V128 r;
    std::memcpy(&r, R, 16);
    return r;
}
RC_INLINE V128 SHA256MessageSchedule1(V128 x, V128 y, V128 z) {
    u32 X[4], Y[4], Z[4], T0[4], R[4];
    std::memcpy(X, &x, 16);
    std::memcpy(Y, &y, 16);
    std::memcpy(Z, &z, 16);
    T0[0] = Y[1], T0[1] = Y[2], T0[2] = Y[3], T0[3] = Z[0];
    auto s1 = [](u32 v) { return Ror32(v, 17) ^ Ror32(v, 19) ^ (v >> 10); };
    R[0] = s1(Z[2]) + X[0] + T0[0];
    R[1] = s1(Z[3]) + X[1] + T0[1];
    R[2] = s1(R[0]) + X[2] + T0[2];
    R[3] = s1(R[1]) + X[3] + T0[3];
    V128 r;
    std::memcpy(&r, R, 16);
    return r;
}

} // namespace op

// AES rounds (dynarmic's table implementation, recomp_fp.cpp)
V128 AESEncryptSingleRound(V128 a);
V128 AESDecryptSingleRound(V128 a);
V128 AESMixColumns(V128 a);
V128 AESInverseMixColumns(V128 a);
namespace op {
using recomp::AESDecryptSingleRound;
using recomp::AESEncryptSingleRound;
using recomp::AESInverseMixColumns;
using recomp::AESMixColumns;
} // namespace op

// per-lane soft-float vector ops
template <typename T, typename F>
RC_INLINE V128 Lanes(V128 a, F f) {
    V128 r{};
    for (u8 i = 0; i < 16 / sizeof(T); i++)
        r = op::SetElem<T>(r, i, T(f(op::GetElem<T>(a, i))));
    return r;
}
template <typename T, typename F>
RC_INLINE V128 Lanes2(V128 a, V128 b, F f) {
    V128 r{};
    for (u8 i = 0; i < 16 / sizeof(T); i++)
        r = op::SetElem<T>(r, i, T(f(op::GetElem<T>(a, i), op::GetElem<T>(b, i))));
    return r;
}
RC_INLINE V128 FPVectorRecipEstimate32(State& s, V128 a, bool) { return Lanes<u32>(a, [&](u32 x) { return FPRecipEstimate32(s, x); }); }
RC_INLINE V128 FPVectorRecipEstimate64(State& s, V128 a, bool) { return Lanes<u64>(a, [&](u64 x) { return FPRecipEstimate64(s, x); }); }
RC_INLINE V128 FPVectorRSqrtEstimate32(State& s, V128 a, bool) { return Lanes<u32>(a, [&](u32 x) { return FPRSqrtEstimate32(s, x); }); }
RC_INLINE V128 FPVectorRSqrtEstimate64(State& s, V128 a, bool) { return Lanes<u64>(a, [&](u64 x) { return FPRSqrtEstimate64(s, x); }); }
RC_INLINE V128 FPVectorRecipStepFused32(State& s, V128 a, V128 b, bool) { return Lanes2<u32>(a, b, [&](u32 x, u32 y) { return FPRecipStepFused32(s, x, y); }); }
RC_INLINE V128 FPVectorRecipStepFused64(State& s, V128 a, V128 b, bool) { return Lanes2<u64>(a, b, [&](u64 x, u64 y) { return FPRecipStepFused64(s, x, y); }); }
RC_INLINE V128 FPVectorRSqrtStepFused32(State& s, V128 a, V128 b, bool) { return Lanes2<u32>(a, b, [&](u32 x, u32 y) { return FPRSqrtStepFused32(s, x, y); }); }
RC_INLINE V128 FPVectorRSqrtStepFused64(State& s, V128 a, V128 b, bool) { return Lanes2<u64>(a, b, [&](u64 x, u64 y) { return FPRSqrtStepFused64(s, x, y); }); }
RC_INLINE V128 FPVectorRoundInt32(State& s, V128 a, u8 r, bool exact, bool) { return Lanes<u32>(a, [&](u32 x) { return FPRoundInt32(s, x, r, exact); }); }
RC_INLINE V128 FPVectorRoundInt64(State& s, V128 a, u8 r, bool exact, bool) { return Lanes<u64>(a, [&](u64 x) { return FPRoundInt64(s, x, r, exact); }); }
RC_INLINE V128 FPVectorToSignedFixed32(State& s, V128 a, u8 f, u8 r, bool) { return Lanes<u32>(a, [&](u32 x) { return FPToFixed32(s, x, 32, f, false, r); }); }
RC_INLINE V128 FPVectorToUnsignedFixed32(State& s, V128 a, u8 f, u8 r, bool) { return Lanes<u32>(a, [&](u32 x) { return FPToFixed32(s, x, 32, f, true, r); }); }
RC_INLINE V128 FPVectorToSignedFixed64(State& s, V128 a, u8 f, u8 r, bool) { return Lanes<u64>(a, [&](u64 x) { return FPToFixed64(s, x, 64, f, false, r); }); }
RC_INLINE V128 FPVectorToUnsignedFixed64(State& s, V128 a, u8 f, u8 r, bool) { return Lanes<u64>(a, [&](u64 x) { return FPToFixed64(s, x, 64, f, true, r); }); }
// ---- saturating SIMD: lanes that saturate set FPSR.QC (bit 27), as the JIT does
constexpr u32 kQC = 1u << 27;
template <typename T>
RC_INLINE T SatClamp(s64 v, bool& q) {
    constexpr s64 lo = s64(std::numeric_limits<T>::min()), hi = s64(std::numeric_limits<T>::max());
    if (v < lo || v > hi) {
        q = true;
        return T(v < lo ? lo : hi);
    }
    return T(v);
}
template <typename T, typename F>
RC_INLINE V128 SatLanes2(State& s, V128 a, V128 b, F f) {
    using U = std::make_unsigned_t<T>;
    V128 r{};
    bool q = false;
    for (u8 i = 0; i < 16 / sizeof(T); i++)
        r = op::SetElem<U>(r, i, U(f(T(op::GetElem<U>(a, i)), T(op::GetElem<U>(b, i)), q)));
    if (q)
        s.fpsr |= kQC;
    return r;
}
template <typename T, typename F>
RC_INLINE V128 SatLanes1(State& s, V128 a, F f) {
    return SatLanes2<T>(s, a, a, [&](T x, T, bool& q) { return f(x, q); });
}
// 64-bit lanes: compute in 128 bits
template <typename T>
RC_INLINE T SatAddS(T x, T y, bool& q) {
    if constexpr (sizeof(T) == 8) {
        const T r = T(u64(x) + u64(y));
        if (((x ^ r) & (y ^ r)) < 0) {
            q = true;
            return x < 0 ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max();
        }
        return r;
    } else {
        return SatClamp<T>(s64(x) + s64(y), q);
    }
}
template <typename T>
RC_INLINE T SatSubS(T x, T y, bool& q) {
    if constexpr (sizeof(T) == 8) {
        const T r = T(u64(x) - u64(y));
        if (((x ^ y) & (x ^ r)) < 0) {
            q = true;
            return x < 0 ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max();
        }
        return r;
    } else {
        return SatClamp<T>(s64(x) - s64(y), q);
    }
}
template <typename T>
RC_INLINE T SatAddU(T x, T y, bool& q) {
    const T r = T(x + y);
    if (r < x) {
        q = true;
        return std::numeric_limits<T>::max();
    }
    return r;
}
template <typename T>
RC_INLINE T SatSubU(T x, T y, bool& q) {
    if (y > x) {
        q = true;
        return 0;
    }
    return T(x - y);
}
#define RC_SAT_BIN(NAME, FN, T)                                                                                       \
    RC_INLINE V128 NAME(State& s, V128 a, V128 b) { return SatLanes2<T>(s, a, b, FN<T>); }
RC_SAT_BIN(VectorSignedSaturatedAdd8, SatAddS, int8_t)
RC_SAT_BIN(VectorSignedSaturatedAdd16, SatAddS, int16_t)
RC_SAT_BIN(VectorSignedSaturatedAdd32, SatAddS, int32_t)
RC_SAT_BIN(VectorSignedSaturatedAdd64, SatAddS, int64_t)
RC_SAT_BIN(VectorSignedSaturatedSub8, SatSubS, int8_t)
RC_SAT_BIN(VectorSignedSaturatedSub16, SatSubS, int16_t)
RC_SAT_BIN(VectorSignedSaturatedSub32, SatSubS, int32_t)
RC_SAT_BIN(VectorSignedSaturatedSub64, SatSubS, int64_t)
RC_SAT_BIN(VectorUnsignedSaturatedAdd8, SatAddU, u8)
RC_SAT_BIN(VectorUnsignedSaturatedAdd16, SatAddU, u16)
RC_SAT_BIN(VectorUnsignedSaturatedAdd32, SatAddU, u32)
RC_SAT_BIN(VectorUnsignedSaturatedAdd64, SatAddU, u64)
RC_SAT_BIN(VectorUnsignedSaturatedSub8, SatSubU, u8)
RC_SAT_BIN(VectorUnsignedSaturatedSub16, SatSubU, u16)
RC_SAT_BIN(VectorUnsignedSaturatedSub32, SatSubU, u32)
RC_SAT_BIN(VectorUnsignedSaturatedSub64, SatSubU, u64)
#undef RC_SAT_BIN
template <typename T>
RC_INLINE V128 SatAbsNeg(State& s, V128 a, bool neg) {
    return SatLanes1<T>(s, a, [neg](T x, bool& q) {
        if (x == std::numeric_limits<T>::min()) {
            q = true;
            return std::numeric_limits<T>::max();
        }
        return T(neg ? -x : (x < 0 ? -x : x));
    });
}
RC_INLINE V128 VectorSignedSaturatedAbs8(State& s, V128 a) { return SatAbsNeg<int8_t>(s, a, false); }
RC_INLINE V128 VectorSignedSaturatedAbs16(State& s, V128 a) { return SatAbsNeg<int16_t>(s, a, false); }
RC_INLINE V128 VectorSignedSaturatedAbs32(State& s, V128 a) { return SatAbsNeg<int32_t>(s, a, false); }
RC_INLINE V128 VectorSignedSaturatedAbs64(State& s, V128 a) { return SatAbsNeg<int64_t>(s, a, false); }
RC_INLINE V128 VectorSignedSaturatedNeg8(State& s, V128 a) { return SatAbsNeg<int8_t>(s, a, true); }
RC_INLINE V128 VectorSignedSaturatedNeg16(State& s, V128 a) { return SatAbsNeg<int16_t>(s, a, true); }
RC_INLINE V128 VectorSignedSaturatedNeg32(State& s, V128 a) { return SatAbsNeg<int32_t>(s, a, true); }
RC_INLINE V128 VectorSignedSaturatedNeg64(State& s, V128 a) { return SatAbsNeg<int64_t>(s, a, true); }
// SQXTN / SQXTUN / UQXTN: narrow every lane into the lower half
template <typename From, typename To>
RC_INLINE V128 SatNarrow(State& s, V128 a) {
    using UF = std::make_unsigned_t<From>;
    using UT = std::make_unsigned_t<To>;
    V128 r{};
    bool q = false;
    for (u8 i = 0; i < 16 / sizeof(From); i++) {
        const From x = From(op::GetElem<UF>(a, i));
        To v;
        if (x < From(std::numeric_limits<To>::min()) && std::is_signed_v<From>) {
            q = true;
            v = std::numeric_limits<To>::min();
        } else if (std::is_signed_v<From> && std::is_unsigned_v<To> && x < 0) {
            q = true;
            v = 0;
        } else if (UF(x) > UF(std::numeric_limits<To>::max()) && (!std::is_signed_v<From> || x >= 0)) {
            q = true;
            v = std::numeric_limits<To>::max();
        } else {
            v = To(x);
        }
        r = op::SetElem<UT>(r, i, UT(v));
    }
    if (q)
        s.fpsr |= kQC;
    return r;
}
RC_INLINE V128 VectorSignedSaturatedNarrowToSigned16(State& s, V128 a) { return SatNarrow<int16_t, int8_t>(s, a); }
RC_INLINE V128 VectorSignedSaturatedNarrowToSigned32(State& s, V128 a) { return SatNarrow<int32_t, int16_t>(s, a); }
RC_INLINE V128 VectorSignedSaturatedNarrowToSigned64(State& s, V128 a) { return SatNarrow<int64_t, int32_t>(s, a); }
RC_INLINE V128 VectorSignedSaturatedNarrowToUnsigned16(State& s, V128 a) { return SatNarrow<int16_t, u8>(s, a); }
RC_INLINE V128 VectorSignedSaturatedNarrowToUnsigned32(State& s, V128 a) { return SatNarrow<int32_t, u16>(s, a); }
RC_INLINE V128 VectorSignedSaturatedNarrowToUnsigned64(State& s, V128 a) { return SatNarrow<int64_t, u32>(s, a); }
RC_INLINE V128 VectorUnsignedSaturatedNarrow16(State& s, V128 a) { return SatNarrow<u16, u8>(s, a); }
RC_INLINE V128 VectorUnsignedSaturatedNarrow32(State& s, V128 a) { return SatNarrow<u32, u16>(s, a); }
RC_INLINE V128 VectorUnsignedSaturatedNarrow64(State& s, V128 a) { return SatNarrow<u64, u32>(s, a); }
// SQDMULH / SQRDMULH: (2*a*b [+ rounding]) >> esize, saturating only for min*min
template <typename T, bool kRound>
RC_INLINE V128 SatDMulH(State& s, V128 a, V128 b) {
    return SatLanes2<T>(s, a, b, [](T x, T y, bool& q) {
        constexpr int kBits = sizeof(T) * 8;
        if (x == std::numeric_limits<T>::min() && y == x) {
            q = true;
            return std::numeric_limits<T>::max();
        }
        s64 p = 2 * s64(x) * s64(y);
        if (kRound)
            p += s64(1) << (kBits - 1);
        return SatClamp<T>(p >> kBits, q);
    });
}
RC_INLINE V128 VectorSignedSaturatedDoublingMultiplyHigh16(State& s, V128 a, V128 b) { return SatDMulH<int16_t, false>(s, a, b); }
RC_INLINE V128 VectorSignedSaturatedDoublingMultiplyHigh32(State& s, V128 a, V128 b) { return SatDMulH<int32_t, false>(s, a, b); }
RC_INLINE V128 VectorSignedSaturatedDoublingMultiplyHighRounding16(State& s, V128 a, V128 b) { return SatDMulH<int16_t, true>(s, a, b); }
RC_INLINE V128 VectorSignedSaturatedDoublingMultiplyHighRounding32(State& s, V128 a, V128 b) { return SatDMulH<int32_t, true>(s, a, b); }
// SQSHL (register): per-lane signed shift by the low byte of the shift lane, saturating left shifts
template <typename T>
RC_INLINE V128 SatShiftLeft(State& s, V128 a, V128 b) {
    using U = std::make_unsigned_t<T>;
    return SatLanes2<T>(s, a, b, [](T x, T sh_lane, bool& q) {
        constexpr T kMax = T(sizeof(T) * 8 - 1);
        const T sh = std::clamp<T>(T(int8_t(u8(U(sh_lane) & 0xFF))), T(-kMax), std::numeric_limits<T>::max());
        const T sat = T((U(x) >> kMax) + (U(1) << kMax) - 1); // max for x >= 0, min for x < 0
        if (x == 0)
            return T(0);
        if (sh < 0)
            return T(x >> -sh);
        if (U(sh) > U(kMax)) {
            q = true;
            return sat;
        }
        const T shifted = T(U(x) << sh);
        if (T(shifted >> sh) != x) {
            q = true;
            return sat;
        }
        return shifted;
    });
}
RC_INLINE V128 VectorSignedSaturatedShiftLeft8(State& s, V128 a, V128 b) { return SatShiftLeft<int8_t>(s, a, b); }
RC_INLINE V128 VectorSignedSaturatedShiftLeft16(State& s, V128 a, V128 b) { return SatShiftLeft<int16_t>(s, a, b); }
RC_INLINE V128 VectorSignedSaturatedShiftLeft32(State& s, V128 a, V128 b) { return SatShiftLeft<int32_t>(s, a, b); }
RC_INLINE V128 VectorSignedSaturatedShiftLeft64(State& s, V128 a, V128 b) { return SatShiftLeft<int64_t>(s, a, b); }
// Scalar SQADD / SQSUB / SQDMULH (the JIT ORs the overflow straight into FPSR.QC as well)
template <typename T, typename F>
RC_INLINE T SatScalar(State& s, T x, T y, F f) {
    bool q = false;
    const T r = f(x, y, q);
    if (q)
        s.fpsr |= kQC;
    return r;
}
RC_INLINE u8 SignedSaturatedAdd8(State& s, u8 a, u8 b) { return u8(SatScalar<int8_t>(s, int8_t(a), int8_t(b), SatAddS<int8_t>)); }
RC_INLINE u16 SignedSaturatedAdd16(State& s, u16 a, u16 b) { return u16(SatScalar<int16_t>(s, int16_t(a), int16_t(b), SatAddS<int16_t>)); }
RC_INLINE u32 SignedSaturatedAdd32(State& s, u32 a, u32 b) { return u32(SatScalar<int32_t>(s, int32_t(a), int32_t(b), SatAddS<int32_t>)); }
RC_INLINE u64 SignedSaturatedAdd64(State& s, u64 a, u64 b) { return u64(SatScalar<int64_t>(s, int64_t(a), int64_t(b), SatAddS<int64_t>)); }
RC_INLINE u8 SignedSaturatedSub8(State& s, u8 a, u8 b) { return u8(SatScalar<int8_t>(s, int8_t(a), int8_t(b), SatSubS<int8_t>)); }
RC_INLINE u16 SignedSaturatedSub16(State& s, u16 a, u16 b) { return u16(SatScalar<int16_t>(s, int16_t(a), int16_t(b), SatSubS<int16_t>)); }
RC_INLINE u32 SignedSaturatedSub32(State& s, u32 a, u32 b) { return u32(SatScalar<int32_t>(s, int32_t(a), int32_t(b), SatSubS<int32_t>)); }
RC_INLINE u64 SignedSaturatedSub64(State& s, u64 a, u64 b) { return u64(SatScalar<int64_t>(s, int64_t(a), int64_t(b), SatSubS<int64_t>)); }
template <typename T>
RC_INLINE T SatDMulHScalar(T x, T y, bool& q) {
    if (x == std::numeric_limits<T>::min() && y == x) {
        q = true;
        return std::numeric_limits<T>::max();
    }
    return T((2 * s64(x) * s64(y)) >> (sizeof(T) * 8));
}
RC_INLINE u16 SignedSaturatedDoublingMultiplyReturnHigh16(State& s, u16 a, u16 b) {
    return u16(SatScalar<int16_t>(s, int16_t(a), int16_t(b), SatDMulHScalar<int16_t>));
}
RC_INLINE u32 SignedSaturatedDoublingMultiplyReturnHigh32(State& s, u32 a, u32 b) {
    return u32(SatScalar<int32_t>(s, int32_t(a), int32_t(b), SatDMulHScalar<int32_t>));
}

} // namespace recomp
