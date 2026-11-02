// recomp: ahead-of-time recompiler for the guest's AArch64 code.
//
//   recomp <module.bin> <load base> <text size> <out dir> [--profile pcs.txt] [--coverage 0.95] [--max-funcs N]
//
// <module.bin> is a flat module image from nsodump (text at offset 0). Function entries come from BL targets, the
// module's relative relocations that point into text (vtables, function pointers) and its function symbols. Each
// basic block is translated with dynarmic's A64 frontend and the IR is emitted as C++ against recomp::State (see
// src/core/cpu/recomp/recomp_rt.h). Blocks using IR opcodes the emitter does not know exit to the JIT.
//
// With --profile (lines "<pc hex> <count>", written by MUA3_RECOMP_PROFILE), only the hottest functions covering
// the given fraction of samples are emitted.
#include <algorithm>
#include <stdexcept>
#include <fmt/format.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dynarmic/frontend/A64/a64_location_descriptor.h>
#include <dynarmic/frontend/A64/translate/a64_translate.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/common/fp/fpcr.h>
#include <dynarmic/ir/acc_type.h>
#include <dynarmic/ir/basic_block.h>
#include <dynarmic/ir/microinstruction.h>
#include <dynarmic/ir/opcodes.h>
#include <dynarmic/ir/opt/passes.h>
#include <dynarmic/ir/terminal.h>

// Garbage in text (literal pools after noreturn calls) can trip dynarmic's asserts: turn them into exceptions so the
// block is just marked unsupported. Defining this here keeps mcl's terminating version out of the link.
namespace mcl::detail {
[[noreturn]] void assert_terminate_impl(const char* expr_str, fmt::string_view, fmt::format_args) {
    throw std::runtime_error(expr_str);
}
} // namespace mcl::detail

using namespace Dynarmic;
using Op = IR::Opcode;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

namespace {

struct Module {
    std::vector<u8> image;
    u64 base, text_end;
};
std::vector<Module> g_mods;
// module being scanned by FindEntries
std::vector<u8> g_image;
u64 g_base = 0, g_text_end = 0;

const Module* ModOf(u64 a) {
    for (const auto& m : g_mods)
        if (a >= m.base && a < m.text_end)
            return &m;
    return nullptr;
}
bool InText(u64 a) { return (a & 3) == 0 && ModOf(a); }
u32 Word(u64 a) {
    const Module* m = ModOf(a);
    u32 w;
    std::memcpy(&w, m->image.data() + (a - m->base), 4);
    return w;
}
template <typename T>
T ImgRead(u64 off) {
    T v{};
    if (off + sizeof(T) <= g_image.size())
        std::memcpy(&v, g_image.data() + off, sizeof(T));
    return v;
}

IR::Block TranslateAt(u64 pc) {
    A64::TranslationOptions opt;
    opt.define_unpredictable_behaviour = true;
    opt.wall_clock_cntpct = true;
    opt.hook_hint_instructions = false;
    IR::Block b = A64::Translate(A64::LocationDescriptor{pc, FP::FPCR{0}},
                                 [](u64 a) -> std::optional<u32> {
                                     if (!InText(a))
                                         return std::nullopt;
                                     return Word(a);
                                 },
                                 opt);
    A64::UserConfig conf{};
    conf.dczid_el0 = 4;
    Optimization::A64CallbackConfigPass(b, conf);
    Optimization::A64GetSetElimination(b);
    Optimization::DeadCodeElimination(b);
    Optimization::ConstantPropagation(b);
    Optimization::DeadCodeElimination(b);
    Optimization::IdentityRemovalPass(b);
    return b;
}

u64 PcOf(const IR::LocationDescriptor& l) { return A64::LocationDescriptor{l}.PC(); }

// ---------------------------------------------------------------- block analysis
struct BlockInfo {
    u64 end = 0;
    std::vector<u64> succ;     // static successors inside the function (incl. call return points)
    std::vector<u64> calls;    // BL targets
    bool supported = true;
    u64 ret = 0;               // call blocks: the return point
    u64 svc_next = 0;          // blocks with an SVC: where the thread continues (a thread halted in the SVC resumes there)
    std::vector<std::string> unsupported_ops;
};

// Switch statements compiled to a jump table ending a block:
//   [adrp xB, page; add xB, xB, #lo] ... cmp wI/xI, #N; b.hi default ... ldrsw xT, [xB, xI, lsl #2];
//   add xT, xT, xB (or xB, xT); br xT
// The table holds N+1 signed 32-bit offsets from its base. Returns the targets (empty if the pattern is not found).
std::vector<u64> JumpTableTargets(u64 br) {
    std::vector<u64> out;
    const u32 w_br = Word(br);
    if ((w_br & 0xFFFFFC1Fu) != 0xD61F0000u)
        return out;
    const u32 t = (w_br >> 5) & 31;
    // ADD xT, xT, xB (shifted register, LSL #0) a few instructions before the BR (others may sit in between, e.g.
    // "mov w8, #1"), then LDRSW xT, [xB, xI, LSL #2] shortly before that.
    auto writes = [](u32 w, u32 r) { return (w & 31) == r && (w & 0x9F000000u) != 0x14000000u; }; // rough: Rd field
    u64 add_at = 0;
    for (u64 a = br - 4; a + 4 * 5 > br && InText(a); a -= 4) {
        const u32 w = Word(a);
        if ((w & 0xFFE0FC00u) == 0x8B000000u && (w & 31) == t) {
            add_at = a;
            break;
        }
        if (writes(w, t))
            return out;
    }
    if (!add_at)
        return out;
    const u32 w_add = Word(add_at);
    const u32 an = (w_add >> 5) & 31, am = (w_add >> 16) & 31;
    u64 ld_at = 0;
    for (u64 a = add_at - 4; a + 4 * 4 > add_at && InText(a); a -= 4) {
        const u32 w = Word(a);
        if ((w & 0xFFE0FC00u) == 0xB8A07800u && (w & 31) == t) { // LDRSW Xt, [Xn, Xm, LSL #2]
            ld_at = a;
            break;
        }
    }
    if (!ld_at)
        return out;
    const u32 w_ld = Word(ld_at);
    const u32 base_reg = (w_ld >> 5) & 31;
    u32 idx_reg = (w_ld >> 16) & 31;
    if (!((an == t && am == base_reg) || (am == t && an == base_reg)))
        return out;
    // Walk back for the bound (CMP idx, #imm then B.HI / B.HS; the index may have been copied with MOV after the
    // check) and the base: ADRP + ADD of base_reg, or ADR. Loops keep the table base in a callee-saved register set
    // up long before, so the base search goes further back than the bound search.
    std::optional<u64> page, lo, adr;
    std::optional<u32> count;
    for (u64 a = ld_at - 4; a + 4 * 600 > ld_at && InText(a); a -= 4) {
        const u32 w = Word(a);
        const bool near = a + 4 * 32 > ld_at;
        if (near && !count) {
            // MOV Wd, Wm (ORR Wd, WZR, Wm) / MOV Xd, Xm into the index: follow the copy
            if ((w & 0x7FE0FFE0u) == 0x2A0003E0u && (w & 31) == idx_reg)
                idx_reg = (w >> 16) & 31;
            // CMP Wn/Xn, #imm12 (SUBS with Rd = zr, no shift), then a B.HI or B.HS right after it
            else if ((w & 0x7FC0001Fu) == 0x7100001Fu && ((w >> 5) & 31) == idx_reg && InText(a + 4)) {
                const u32 n = (w >> 10) & 0xFFF, next = Word(a + 4);
                if ((next & 0xFF00001Fu) == 0x54000008u) // B.HI
                    count = n + 1;
                else if ((next & 0xFF00001Fu) == 0x54000002u) // B.HS
                    count = n;
            }
        }
        if (!lo && !adr && (w & 0xFFC00000u) == 0x91000000u && (w & 31) == base_reg && ((w >> 5) & 31) == base_reg)
            lo = (w >> 10) & 0xFFF; // ADD xB, xB, #imm12
        else if (lo && !page && (w & 0x9F000000u) == 0x90000000u && (w & 31) == base_reg) { // ADRP xB
            const int64_t imm = int64_t(int32_t((((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)) << 11) >> 11);
            page = (a & ~0xFFFull) + u64(imm << 12);
        } else if (!lo && !adr && (w & 0x9F000000u) == 0x10000000u && (w & 31) == base_reg) { // ADR xB
            const int64_t imm = int64_t(int32_t((((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)) << 11) >> 11);
            adr = a + u64(imm);
        }
        if (count && (adr || (page && lo)))
            break;
        if (!near && !count)
            break; // no bound close to the load: not a recognisable switch
    }
    if (!count || *count == 0 || *count > 4096 || !(adr || (page && lo)))
        return out;
    const u64 table = adr ? *adr : *page + *lo;
    const Module* m = ModOf(br);
    for (u32 k = 0; k < *count; k++) {
        const u64 at = table + u64(k) * 4;
        if (at < m->base || at + 4 > m->base + m->image.size())
            return {};
        int32_t off;
        std::memcpy(&off, m->image.data() + (at - m->base), 4);
        const u64 target = table + u64(int64_t(off));
        // a wrong base gives scattered garbage: real cases sit near their switch and start with code
        if (!InText(target) || (target > br ? target - br : br - target) > (1u << 20) || Word(target) == 0)
            return {};
        out.push_back(target);
    }
    return out;
}
size_t g_jump_tables = 0, g_jump_targets = 0;

const std::unordered_set<std::string>& GenericOps() {
    static const std::unordered_set<std::string> ops = {
        "Pack2x32To1x64", "Pack2x64To1x128", "LeastSignificantWord", "LeastSignificantHalf", "LeastSignificantByte",
        "MostSignificantWord", "MostSignificantBit", "IsZero32", "IsZero64", "TestBit", "ConditionalSelect32",
        "ConditionalSelect64", "ConditionalSelectNZCV", "NZCVFromPackedFlags", "GetCFlagFromNZCV",
        "LogicalShiftLeft32", "LogicalShiftLeft64", "LogicalShiftRight32", "LogicalShiftRight64",
        "ArithmeticShiftRight32", "ArithmeticShiftRight64", "RotateRight32", "RotateRight64",
        "LogicalShiftLeftMasked32", "LogicalShiftLeftMasked64", "LogicalShiftRightMasked32",
        "LogicalShiftRightMasked64", "ArithmeticShiftRightMasked32", "ArithmeticShiftRightMasked64",
        "RotateRightMasked32", "RotateRightMasked64", "Add32", "Add64", "Sub32", "Sub64", "Mul32", "Mul64",
        "SignedMultiplyHigh64", "UnsignedMultiplyHigh64", "UnsignedDiv32", "UnsignedDiv64", "SignedDiv32",
        "SignedDiv64", "And32", "And64", "AndNot32", "AndNot64", "Eor32", "Eor64", "Or32", "Or64", "Not32", "Not64",
        "SignExtendByteToWord", "SignExtendHalfToWord", "SignExtendByteToLong", "SignExtendHalfToLong",
        "SignExtendWordToLong", "ZeroExtendByteToWord", "ZeroExtendHalfToWord", "ZeroExtendByteToLong",
        "ZeroExtendHalfToLong", "ZeroExtendWordToLong", "ZeroExtendLongToQuad", "ByteReverseWord",
        "ByteReverseHalf", "ByteReverseDual", "CountLeadingZeros32", "CountLeadingZeros64", "ExtractRegister32",
        "ExtractRegister64", "ReplicateBit32", "ReplicateBit64", "MaxSigned32", "MaxSigned64", "MaxUnsigned32",
        "MaxUnsigned64", "MinSigned32", "MinSigned64", "MinUnsigned32", "MinUnsigned64", "CRC32Castagnoli8",
        "CRC32Castagnoli16", "CRC32Castagnoli32", "CRC32Castagnoli64", "CRC32ISO8", "CRC32ISO16", "CRC32ISO32",
        "CRC32ISO64", "VectorGetElement8", "VectorGetElement16", "VectorGetElement32", "VectorGetElement64",
        "VectorSetElement8", "VectorSetElement16", "VectorSetElement32", "VectorSetElement64", "VectorZeroUpper",
        "ZeroVector", "VectorAnd", "VectorAndNot", "VectorOr", "VectorEor", "VectorNot", "VectorBroadcast8",
        "VectorBroadcast16", "VectorBroadcast32", "VectorBroadcast64", "VectorBroadcastLower8",
        "VectorBroadcastLower16", "VectorBroadcastLower32",
        "FPAbs32", "FPAbs64", "FPNeg32", "FPNeg64", "FPAdd32", "FPAdd64", "FPSub32", "FPSub64", "FPMul32",
        "FPMul64", "FPDiv32", "FPDiv64", "FPSqrt32", "FPSqrt64", "FPMulAdd32", "FPMulAdd64", "FPMulSub32",
        "FPMulSub64", "FPCompare32", "FPCompare64", "FPMax32", "FPMax64", "FPMin32", "FPMin64", "FPMaxNumeric32",
        "FPMaxNumeric64", "FPMinNumeric32", "FPMinNumeric64", "FPMulX32", "FPMulX64", "FPSingleToDouble",
        "FPDoubleToSingle", "FPFixedU16ToSingle", "FPFixedU16ToDouble", "FPFixedS16ToSingle", "FPFixedS16ToDouble",
        "FPFixedU32ToSingle", "FPFixedU32ToDouble", "FPFixedS32ToSingle", "FPFixedS32ToDouble",
        "FPFixedU64ToSingle", "FPFixedU64ToDouble", "FPFixedS64ToSingle", "FPFixedS64ToDouble",
        "FPVectorAbs32", "FPVectorAbs64", "FPVectorAdd32", "FPVectorAdd64", "FPVectorDiv32", "FPVectorDiv64",
        "FPVectorEqual32", "FPVectorEqual64", "FPVectorFromSignedFixed32", "FPVectorFromSignedFixed64",
        "FPVectorFromUnsignedFixed32", "FPVectorFromUnsignedFixed64", "FPVectorGreater32", "FPVectorGreater64",
        "FPVectorGreaterEqual32", "FPVectorGreaterEqual64", "FPVectorMax32", "FPVectorMax64",
        "FPVectorMaxNumeric32", "FPVectorMaxNumeric64", "FPVectorMin32", "FPVectorMin64", "FPVectorMinNumeric32",
        "FPVectorMinNumeric64", "FPVectorMul32", "FPVectorMul64", "FPVectorMulAdd32", "FPVectorMulAdd64",
        "FPVectorMulX32", "FPVectorMulX64", "FPVectorNeg32", "FPVectorNeg64", "FPVectorPairedAdd32",
        "FPVectorPairedAdd64", "FPVectorPairedAddLower32", "FPVectorPairedAddLower64", "FPVectorSqrt32",
        "FPVectorSqrt64", "FPVectorSub32", "FPVectorSub64", "VectorAdd16", "VectorAdd32", "VectorAdd64",
        "VectorAdd8", "VectorArithmeticShiftRight16", "VectorArithmeticShiftRight32",
        "VectorArithmeticShiftRight64", "VectorArithmeticShiftRight8", "VectorBroadcastElement16",
        "VectorBroadcastElement32", "VectorBroadcastElement64", "VectorBroadcastElement8",
        "VectorBroadcastElementLower16", "VectorBroadcastElementLower32", "VectorBroadcastElementLower8",
        "VectorEqual128", "VectorEqual16", "VectorEqual32", "VectorEqual64", "VectorEqual8", "VectorExtract",
        "VectorExtractLower", "VectorGreaterS16", "VectorGreaterS32", "VectorGreaterS64", "VectorGreaterS8",
        "VectorInterleaveLower16", "VectorInterleaveLower32", "VectorInterleaveLower64", "VectorInterleaveLower8",
        "VectorInterleaveUpper16", "VectorInterleaveUpper32", "VectorInterleaveUpper64", "VectorInterleaveUpper8",
        "VectorLogicalShiftLeft16", "VectorLogicalShiftLeft32", "VectorLogicalShiftLeft64",
        "VectorLogicalShiftLeft8", "VectorLogicalShiftRight16", "VectorLogicalShiftRight32",
        "VectorLogicalShiftRight64", "VectorLogicalShiftRight8", "VectorMaxS16", "VectorMaxS32", "VectorMaxS8",
        "VectorMaxU16", "VectorMaxU32", "VectorMaxU8", "VectorMinS16", "VectorMinS32", "VectorMinS8",
        "VectorMinU16", "VectorMinU32", "VectorMinU8", "VectorMultiply16", "VectorMultiply32", "VectorNarrow16",
        "VectorNarrow32", "VectorNarrow64", "VectorSignExtend16", "VectorSignExtend32", "VectorSignExtend8",
        "VectorSub16", "VectorSub32", "VectorSub64", "VectorSub8", "VectorZeroExtend16", "VectorZeroExtend32",
        "VectorZeroExtend64", "VectorZeroExtend8",
        "VectorTableLookup128", "VectorLogicalVShift8", "VectorLogicalVShift16", "VectorLogicalVShift32",
        "VectorLogicalVShift64", "VectorArithmeticVShift8", "VectorArithmeticVShift16", "VectorArithmeticVShift32",
        "VectorArithmeticVShift64", "VectorRotateWholeVectorRight",
        "SHA256Hash", "SHA256MessageSchedule0", "SHA256MessageSchedule1", "VectorMultiplySignedWiden16",
        "VectorMultiplySignedWiden32", "VectorMultiplySignedWiden8", "VectorPolynomialMultiplyLong64",
        "VectorPopulationCount", "VectorReduceAdd16", "VectorReduceAdd32", "VectorReduceAdd64", "VectorReduceAdd8",
        "VectorReverseElementsInHalfGroups8", "VectorReverseElementsInLongGroups16",
        "VectorReverseElementsInLongGroups32", "VectorReverseElementsInLongGroups8",
        "VectorReverseElementsInWordGroups16", "VectorReverseElementsInWordGroups8", "VectorTranspose16",
        "VectorTranspose32", "VectorTranspose64", "VectorTranspose8", "AESEncryptSingleRound",
        "AESDecryptSingleRound", "AESMixColumns", "AESInverseMixColumns",
    };
    return ops;
}

// Generic opcodes implemented as runtime functions taking the State (soft-float with FPCR): Name(s, args...).
const std::unordered_set<std::string>& StateOps() {
    static const std::unordered_set<std::string> ops = {
        "FPRoundInt32", "FPRoundInt64", "FPRecipEstimate32", "FPRecipEstimate64", "FPRSqrtEstimate32",
        "FPRSqrtEstimate64", "FPRecipStepFused32", "FPRecipStepFused64", "FPRSqrtStepFused32", "FPRSqrtStepFused64",
        "FPRecipExponent32", "FPRecipExponent64", "FPHalfToSingle", "FPHalfToDouble", "FPSingleToHalf",
        "FPVectorRecipEstimate32", "FPVectorRecipEstimate64", "FPVectorRSqrtEstimate32", "FPVectorRSqrtEstimate64",
        "FPVectorRecipStepFused32", "FPVectorRecipStepFused64", "FPVectorRSqrtStepFused32",
        "FPVectorRSqrtStepFused64", "FPVectorRoundInt32", "FPVectorRoundInt64", "FPVectorToSignedFixed32",
        "FPVectorToUnsignedFixed32", "FPVectorToSignedFixed64", "FPVectorToUnsignedFixed64",
        "NAME",
        "VectorSignedSaturatedAdd8",
        "VectorSignedSaturatedAdd16",
        "VectorSignedSaturatedAdd32",
        "VectorSignedSaturatedAdd64",
        "VectorSignedSaturatedSub8",
        "VectorSignedSaturatedSub16",
        "VectorSignedSaturatedSub32",
        "VectorSignedSaturatedSub64",
        "VectorUnsignedSaturatedAdd8",
        "VectorUnsignedSaturatedAdd16",
        "VectorUnsignedSaturatedAdd32",
        "VectorUnsignedSaturatedAdd64",
        "VectorUnsignedSaturatedSub8",
        "VectorUnsignedSaturatedSub16",
        "VectorUnsignedSaturatedSub32",
        "VectorUnsignedSaturatedSub64",
        "VectorSignedSaturatedAbs8",
        "VectorSignedSaturatedAbs16",
        "VectorSignedSaturatedAbs32",
        "VectorSignedSaturatedAbs64",
        "VectorSignedSaturatedNeg8",
        "VectorSignedSaturatedNeg16",
        "VectorSignedSaturatedNeg32",
        "VectorSignedSaturatedNeg64",
        "VectorSignedSaturatedNarrowToSigned16",
        "VectorSignedSaturatedNarrowToSigned32",
        "VectorSignedSaturatedNarrowToSigned64",
        "VectorSignedSaturatedNarrowToUnsigned16",
        "VectorSignedSaturatedNarrowToUnsigned32",
        "VectorSignedSaturatedNarrowToUnsigned64",
        "VectorUnsignedSaturatedNarrow16",
        "VectorUnsignedSaturatedNarrow32",
        "VectorUnsignedSaturatedNarrow64",
        "VectorSignedSaturatedDoublingMultiplyHigh16",
        "VectorSignedSaturatedDoublingMultiplyHigh32",
        "VectorSignedSaturatedDoublingMultiplyHighRounding16",
        "VectorSignedSaturatedDoublingMultiplyHighRounding32",
        "VectorSignedSaturatedShiftLeft8", "VectorSignedSaturatedShiftLeft16", "VectorSignedSaturatedShiftLeft32",
        "VectorSignedSaturatedShiftLeft64", "SignedSaturatedAdd8", "SignedSaturatedAdd16", "SignedSaturatedAdd32",
        "SignedSaturatedAdd64", "SignedSaturatedSub8", "SignedSaturatedSub16", "SignedSaturatedSub32",
        "SignedSaturatedSub64", "SignedSaturatedDoublingMultiplyReturnHigh16",
        "SignedSaturatedDoublingMultiplyReturnHigh32",
        "FPDoubleToHalf", "FPSingleToFixedS16", "FPSingleToFixedS32", "FPSingleToFixedS64", "FPSingleToFixedU16",
        "FPSingleToFixedU32", "FPSingleToFixedU64", "FPDoubleToFixedS16", "FPDoubleToFixedS32",
        "FPDoubleToFixedS64", "FPDoubleToFixedU16", "FPDoubleToFixedU32", "FPDoubleToFixedU64",
    };
    return ops;
}

// Opcodes the emitter handles itself (state access, memory, system).
bool IsSpecialSupported(Op o) {
    switch (o) {
    case Op::Void:
    case Op::Identity:
    case Op::A64SetCheckBit:
    case Op::A64GetCFlag:
    case Op::A64GetNZCVRaw:
    case Op::A64SetNZCVRaw:
    case Op::A64SetNZCV:
    case Op::A64GetW:
    case Op::A64GetX:
    case Op::A64GetS:
    case Op::A64GetD:
    case Op::A64GetQ:
    case Op::A64GetSP:
    case Op::A64GetFPCR:
    case Op::A64GetFPSR:
    case Op::A64SetW:
    case Op::A64SetX:
    case Op::A64SetS:
    case Op::A64SetD:
    case Op::A64SetQ:
    case Op::A64SetSP:
    case Op::A64SetFPSR:
    case Op::A64SetFPCR:
    case Op::A64SetPC:
    case Op::A64CallSupervisor:
    case Op::A64DataCacheOperationRaised:
    case Op::A64InstructionCacheOperationRaised:
    case Op::A64DataSynchronizationBarrier:
    case Op::A64DataMemoryBarrier:
    case Op::A64InstructionSynchronizationBarrier:
    case Op::A64GetCNTFRQ:
    case Op::A64GetCNTPCT:
    case Op::A64GetCTR:
    case Op::A64GetDCZID:
    case Op::A64GetTPIDR:
    case Op::A64GetTPIDRRO:
    case Op::A64SetTPIDR:
    case Op::PushRSB:
    case Op::A64ClearExclusive:
    case Op::A64ReadMemory8:
    case Op::A64ReadMemory16:
    case Op::A64ReadMemory32:
    case Op::A64ReadMemory64:
    case Op::A64ReadMemory128:
    case Op::A64ExclusiveReadMemory8:
    case Op::A64ExclusiveReadMemory16:
    case Op::A64ExclusiveReadMemory32:
    case Op::A64ExclusiveReadMemory64:
    case Op::A64ExclusiveReadMemory128:
    case Op::A64WriteMemory8:
    case Op::A64WriteMemory16:
    case Op::A64WriteMemory32:
    case Op::A64WriteMemory64:
    case Op::A64WriteMemory128:
    case Op::A64ExclusiveWriteMemory8:
    case Op::A64ExclusiveWriteMemory16:
    case Op::A64ExclusiveWriteMemory32:
    case Op::A64ExclusiveWriteMemory64:
    case Op::A64ExclusiveWriteMemory128:
    case Op::GetCarryFromOp:
    case Op::GetOverflowFromOp:
    case Op::GetNZCVFromOp:
        return true;
    case Op::VectorTable: // only 128-bit tables (TBL/TBX on Q registers); 64-bit lookups stay on the JIT
        return true;
    default:
        return false;
    }
}

bool IsAddSub(Op o) { return o == Op::Add32 || o == Op::Add64 || o == Op::Sub32 || o == Op::Sub64; }

struct BlockShape {
    bool has_push_rsb = false, has_svc = false;
    u64 ret_loc = 0;
    std::optional<u64> setpc_imm; // last SetPC, when its value is an immediate
};
BlockShape Shape(const IR::Block& b) {
    BlockShape sh;
    for (const auto& inst : b) {
        switch (inst.GetOpcode()) {
        case Op::PushRSB:
            sh.has_push_rsb = true;
            sh.ret_loc = PcOf(IR::LocationDescriptor{inst.GetArg(0).GetU64()});
            break;
        case Op::A64CallSupervisor:
            sh.has_svc = true;
            break;
        case Op::A64SetPC:
            if (inst.GetArg(0).IsImmediate())
                sh.setpc_imm = inst.GetArg(0).GetU64();
            else
                sh.setpc_imm.reset();
            break;
        default:
            break;
        }
    }
    return sh;
}

bool IsDynamic(const IR::Terminal& t) {
    return boost::get<IR::Term::PopRSBHint>(&t) || boost::get<IR::Term::FastDispatchHint>(&t) ||
           boost::get<IR::Term::ReturnToDispatch>(&t);
}

void CollectTargets(const IR::Terminal& t, const BlockShape& sh, bool is_call, BlockInfo& bi) {
    if (auto* l = boost::get<IR::Term::LinkBlock>(&t)) {
        (is_call ? bi.calls : bi.succ).push_back(PcOf(l->next));
    } else if (auto* l = boost::get<IR::Term::LinkBlockFast>(&t)) {
        (is_call ? bi.calls : bi.succ).push_back(PcOf(l->next));
    } else if (auto* i = boost::get<IR::Term::If>(&t)) {
        CollectTargets(i->then_, sh, is_call, bi);
        CollectTargets(i->else_, sh, is_call, bi);
    } else if (auto* c = boost::get<IR::Term::CheckBit>(&t)) {
        CollectTargets(c->then_, sh, is_call, bi);
        CollectTargets(c->else_, sh, is_call, bi);
    } else if (auto* h = boost::get<IR::Term::CheckHalt>(&t)) {
        CollectTargets(h->else_, sh, is_call, bi);
    } else if (IsDynamic(t) && sh.setpc_imm && !is_call) {
        bi.succ.push_back(*sh.setpc_imm);
    }
}

std::unordered_map<u64, BlockInfo> g_blocks;
size_t g_emit_failures = 0;

const BlockInfo& Analyze(u64 pc) {
    auto it = g_blocks.find(pc);
    if (it != g_blocks.end())
        return it->second;
    BlockInfo bi;
    std::optional<IR::Block> tb;
    try {
        tb.emplace(TranslateAt(pc));
    } catch (const std::exception&) {
        bi.end = pc + 4;
        bi.supported = false;
        bi.unsupported_ops.push_back("(translation failed)");
        return g_blocks.emplace(pc, std::move(bi)).first->second;
    }
    IR::Block& b = *tb;
    bi.end = PcOf(b.EndLocation());
    for (const auto& inst : b) {
        const Op o = inst.GetOpcode();
        if (IsSpecialSupported(o) || GenericOps().count(IR::GetNameOf(o)) || StateOps().count(IR::GetNameOf(o)))
            continue;
        bi.supported = false;
        bi.unsupported_ops.push_back(IR::GetNameOf(o));
    }
    const BlockShape sh = Shape(b);
    const bool is_call = sh.has_push_rsb && !sh.has_svc;
    const IR::Terminal term = b.GetTerminal();
    if (boost::get<IR::Term::Interpret>(&term) || boost::get<IR::Term::Invalid>(&term))
        bi.supported = false;
    CollectTargets(term, sh, is_call, bi);
    if (is_call)
    {
        bi.succ.push_back(sh.ret_loc);
        bi.ret = sh.ret_loc;
    }
    if (sh.has_svc && sh.setpc_imm)
        bi.svc_next = *sh.setpc_imm;
    // A register branch through a jump table: its targets are blocks of this function.
    if (!is_call && IsDynamic(term) && !sh.setpc_imm && bi.end >= pc + 4) {
        if (const auto targets = JumpTableTargets(bi.end - 4); !targets.empty()) {
            g_jump_tables++, g_jump_targets += targets.size();
            bi.succ.insert(bi.succ.end(), targets.begin(), targets.end());
        }
    }
    return g_blocks.emplace(pc, std::move(bi)).first->second;
}

// ---------------------------------------------------------------- functions
struct Function {
    u64 entry;
    std::vector<u64> blocks; // sorted
    double weight = 0;
};

std::set<u64> g_entries;
// C library routines (by symbol) whose generated function is a call into the host's version: the guest's are
// hand-unrolled NEON loops that cost dozens of emitted ops per 16 bytes.
std::map<u64, std::string> g_intrinsics;
const std::set<std::string>& IntrinsicNames() {
    static const std::set<std::string> n = {"memcpy", "memmove", "memset", "memcmp", "strlen",
                                            "strcmp", "strncmp", "memchr", "strchr"};
    return n;
}
constexpr size_t kMaxBlocks = 20000;

Function Discover(u64 entry) {
    Function f{entry};
    std::set<u64> seen{entry};
    std::vector<u64> work{entry};
    while (!work.empty() && seen.size() < kMaxBlocks) {
        const u64 pc = work.back();
        work.pop_back();
        f.blocks.push_back(pc);
        const BlockInfo& bi = Analyze(pc);
        for (u64 s : bi.succ) {
            if (!InText(s) || seen.count(s) || (g_entries.count(s) && s != entry))
                continue;
            seen.insert(s);
            work.push_back(s);
        }
    }
    std::sort(f.blocks.begin(), f.blocks.end());
    return f;
}

void FindEntries(u64 text_size) {
    // BL targets
    for (u64 a = g_base; a < g_text_end; a += 4) {
        const u32 w = Word(a);
        if ((w & 0xFC000000u) == 0x94000000u) {
            const int64_t off = int64_t(int32_t(w << 6) >> 6) * 4;
            const u64 t = a + off;
            if (InText(t))
                g_entries.insert(t);
        }
    }
    const size_t from_bl = g_entries.size();
    // Code addresses materialised with ADRP + ADD (thread entry points, callbacks handed to the OS): PIC code needs no
    // relocation for them, so they are neither BL targets nor relocations. Accepted when the target starts a
    // function by the usual layout: the word before it ends the previous one (RET, B, BR, UDF/padding, BRK, NOP).
    auto ends_function = [](u32 w) {
        return w == 0xD65F03C0u ||                 // RET
               (w & 0xFC000000u) == 0x14000000u || // B
               (w & 0xFFFFFC1Fu) == 0xD61F0000u || // BR
               w == 0 ||                           // UDF #0 / padding
               (w & 0xFFE0001Fu) == 0xD4200000u || // BRK
               w == 0xD503201Fu;                   // NOP
    };
    size_t from_adrp = 0;
    for (u64 a = g_base; a < g_text_end; a += 4) {
        const u32 w = Word(a);
        if ((w & 0x9F000000u) != 0x90000000u) // ADRP
            continue;
        const u32 rd = w & 31;
        const int64_t imm = int64_t(int32_t((((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)) << 11) >> 11);
        const u64 page = (a & ~0xFFFull) + u64(imm << 12);
        for (u64 b = a + 4; b < a + 4 * 8 && b < g_text_end; b += 4) {
            const u32 x = Word(b);
            // ADD Xd, Xrd, #imm12 (64-bit, shift 0)
            if ((x & 0xFFC00000u) == 0x91000000u && ((x >> 5) & 31) == rd) {
                const u64 t = page + ((x >> 10) & 0xFFF);
                if (InText(t) && Word(t) != 0 && ends_function(Word(t - 4)) && g_entries.insert(t).second)
                    from_adrp++;
                break;
            }
            if ((x & 31) == rd && (x & 0x9F000000u) != 0x90000000u)
                break; // rd overwritten by something else
        }
    }
    // MOD0 -> dynamic -> RELA (relative relocations into text) and function symbols
    const u32 mod0 = ImgRead<u32>(4);
    if (ImgRead<u32>(mod0) != 0x30444F4D) {
        std::printf("no MOD0\n");
        return;
    }
    const u64 dyn = mod0 + int32_t(ImgRead<u32>(mod0 + 4));
    u64 rela = 0, relasz = 0, symtab = 0, strtab = 0, hash = 0;
    for (u64 p = dyn; p < g_image.size(); p += 16) {
        const u64 tag = ImgRead<u64>(p), val = ImgRead<u64>(p + 8);
        if (tag == 0)
            break;
        if (tag == 7)
            rela = val;
        else if (tag == 8)
            relasz = val;
        else if (tag == 6)
            symtab = val;
        else if (tag == 4)
            hash = val;
        else if (tag == 5)
            strtab = val;
    }
    size_t from_rel = 0, from_sym = 0;
    for (u64 p = rela; p + 24 <= rela + relasz; p += 24) {
        if ((ImgRead<u64>(p + 8) & 0xFFFFFFFF) != 1027) // R_AARCH64_RELATIVE
            continue;
        const u64 t = g_base + ImgRead<u64>(p + 16);
        if (InText(t) && g_entries.insert(t).second)
            from_rel++;
    }
    if (symtab && hash) {
        const u32 nchain = ImgRead<u32>(hash + 4);
        for (u32 i = 0; i < nchain; i++) {
            const u64 s = symtab + u64(i) * 24;
            const u8 info = ImgRead<u8>(s + 4);
            const u64 value = ImgRead<u64>(s + 8);
            if ((info & 0xF) == 2 && value && InText(g_base + value) && g_entries.insert(g_base + value).second)
                from_sym++;
            if ((info & 0xF) == 2 && value && strtab) {
                std::string name;
                for (u64 p = strtab + ImgRead<u32>(s); p < g_image.size() && ImgRead<u8>(p); p++)
                    name += char(ImgRead<u8>(p));
                if (IntrinsicNames().count(name))
                    g_intrinsics[g_base + value] = name;
            }
        }
    }
    std::printf("entries: %zu from BL, %zu from ADRP+ADD, %zu from relocations, %zu from symbols\n", from_bl, from_adrp,
                from_rel, from_sym);
}

// ---------------------------------------------------------------- emission
struct Emitter {
    std::ostringstream& out;
    const Function& f;
    const std::unordered_set<u64>& blockset;
    const std::unordered_map<u64, u32>& gen_index;
    std::ostringstream o; // current block
    std::unordered_map<const IR::Inst*, unsigned> names;
    unsigned next_name = 0;
    u64 cur = 0;
    BlockShape sh;
    bool is_call = false;
    std::set<u64>* called = nullptr; // generated functions this shard calls directly (for its declarations)
    // Block-local register promotion: the guest registers, SP and NZCV a block touches live in locals (MSVC
    // cannot keep s.x[] in host registers across the volatile memory accesses); they are loaded at the block's
    // start and written back before anything that observes the State (calls, SVCs, returns, jumps).
    std::set<int> used_regs, dirty_regs;
    bool sp_used = false, sp_dirty = false, nz_used = false, nz_dirty = false;
    std::string R(int r) {
        used_regs.insert(r);
        return "x_" + std::to_string(r);
    }
    std::string RW(int r) {
        dirty_regs.insert(r);
        return R(r);
    }
    std::string SpR() {
        sp_used = true;
        return "x_sp";
    }
    std::string SpW() {
        sp_dirty = true;
        return SpR();
    }
    std::string NzR() {
        nz_used = true;
        return "nzcv";
    }
    std::string NzW() {
        nz_dirty = true;
        return NzR();
    }
    std::string FlushText(const std::string& pad) const {
        std::string t;
        for (int r : dirty_regs)
            t += pad + "s.x[" + std::to_string(r) + "] = x_" + std::to_string(r) + ";\n";
        if (sp_dirty)
            t += pad + "s.sp = x_sp;\n";
        if (nz_dirty)
            t += pad + "s.nzcv = nzcv;\n";
        return t;
    }
    std::string LoadText(bool declare) const {
        std::string t;
        for (int r : used_regs)
            t += (declare ? "  u64 x_" : "  x_") + std::to_string(r) + " = s.x[" + std::to_string(r) + "];\n";
        if (sp_used)
            t += std::string(declare ? "  u64 x_sp" : "  x_sp") + " = s.sp;\n";
        if (nz_used)
            t += std::string(declare ? "  u32 nzcv" : "  nzcv") + " = s.nzcv;\n";
        return t;
    }
    void Flush(const std::string& pad) { o << FlushText(pad); }

    std::string Fn(u64 pc) const {
        if (called)
            called->insert(pc);
        return fmt("f_%llx", pc);
    }
    static std::string fmt(const char* f, unsigned long long v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), f, v);
        return buf;
    }
    std::string Hex(u64 v) const { return fmt("0x%llxull", v); }

    std::string Arg(const IR::Value& v) {
        if (v.IsImmediate()) {
            switch (v.GetType()) {
            case IR::Type::U1: return v.GetU1() ? "true" : "false";
            case IR::Type::U8: return fmt("u8(%llu)", v.GetU8());
            case IR::Type::U16: return fmt("u16(%llu)", v.GetU16());
            case IR::Type::U32: return fmt("0x%llxu", v.GetU32());
            case IR::Type::U64: return Hex(v.GetU64());
            case IR::Type::A64Reg: return std::to_string(int(v.GetA64RegRef()));
            case IR::Type::A64Vec: return std::to_string(int(v.GetA64VecRef()));
            case IR::Type::Cond: return std::to_string(int(v.GetCond()));
            case IR::Type::AccType: return std::to_string(int(v.GetAccType()));
            default: throw std::runtime_error("immediate type");
            }
        }
        const IR::Inst* i = v.GetInst();
        auto it = names.find(i);
        if (it == names.end())
            throw std::runtime_error("use before def");
        return fmt("v%llu", it->second);
    }
    std::string Def(const IR::Inst& i) {
        const unsigned n = next_name++;
        names[&i] = n;
        return fmt("v%llu", n);
    }

    std::string cur_op;
    void Inst(const IR::Inst& in) {
        const Op op = in.GetOpcode();
        cur_op = IR::GetNameOf(op);
        auto A = [&](size_t k) { return Arg(in.GetArg(k)); };
        auto ordered = [&](size_t k) {
            const auto acc = in.GetArg(k).GetAccType();
            return acc == IR::AccType::ORDERED || acc == IR::AccType::ORDEREDRW || acc == IR::AccType::LIMITEDORDERED;
        };
        switch (op) {
        case Op::Void: return;
        case Op::Identity: o << "auto " << Def(in) << " = " << A(0) << ";\n"; return;
        case Op::A64SetCheckBit: o << "cbit = " << A(0) << ";\n"; return;
        case Op::A64GetCFlag: o << "bool " << Def(in) << " = (" << NzR() << " & kC) != 0;\n"; return;
        case Op::A64GetNZCVRaw: o << "u32 " << Def(in) << " = " << NzR() << ";\n"; return;
        case Op::A64SetNZCVRaw: o << NzW() << " = " << A(0) << " & 0xF0000000u;\n"; return;
        case Op::A64SetNZCV: o << NzW() << " = " << A(0) << ";\n"; return;
        case Op::A64GetW: o << "u32 " << Def(in) << " = u32(" << R(int(in.GetArg(0).GetA64RegRef())) << ");\n"; return;
        case Op::A64GetX: o << "u64 " << Def(in) << " = " << R(int(in.GetArg(0).GetA64RegRef())) << ";\n"; return;
        case Op::A64GetS: o << "V128 " << Def(in) << "{u64(u32(s.v[" << A(0) << "].lo)), 0};\n"; return;
        case Op::A64GetD: o << "V128 " << Def(in) << "{s.v[" << A(0) << "].lo, 0};\n"; return;
        case Op::A64GetQ: o << "V128 " << Def(in) << " = s.v[" << A(0) << "];\n"; return;
        case Op::A64GetSP: o << "u64 " << Def(in) << " = " << SpR() << ";\n"; return;
        case Op::A64GetFPCR: o << "u32 " << Def(in) << " = s.fpcr;\n"; return;
        case Op::A64GetFPSR: o << "u32 " << Def(in) << " = s.fpsr;\n"; return;
        case Op::A64SetW: o << RW(int(in.GetArg(0).GetA64RegRef())) << " = u64(" << A(1) << ");\n"; return;
        case Op::A64SetX: o << RW(int(in.GetArg(0).GetA64RegRef())) << " = " << A(1) << ";\n"; return;
        case Op::A64SetS: o << "s.v[" << A(0) << "] = V128{u64(u32(" << A(1) << ".lo)), 0};\n"; return;
        case Op::A64SetD: o << "s.v[" << A(0) << "] = V128{" << A(1) << ".lo, 0};\n"; return;
        case Op::A64SetQ: o << "s.v[" << A(0) << "] = " << A(1) << ";\n"; return;
        case Op::A64SetSP: o << SpW() << " = " << A(0) << ";\n"; return;
        case Op::A64SetFPSR: o << "s.fpsr = " << A(0) << ";\n"; return;
        case Op::A64SetFPCR: o << "SetFpcr(s, " << A(0) << ");\n"; return;
        case Op::A64SetPC: o << "s.pc = " << A(0) << ";\n"; return;
        case Op::A64CallSupervisor:
            // Handlers may redirect the thread (SetPC) or stop it (Halt).
            // (the handler reads and writes the State: write the locals back first, reload them after)
            o << "\n" << FlushText("  ") << "  Svc(s, " << A(0) << "); if (*s.halt || s.pc != " << Hex(sh.setpc_imm.value_or(0))
              << ") return;\n@RELOAD@";
            return;
        case Op::A64DataCacheOperationRaised:
        case Op::A64InstructionCacheOperationRaised:
        case Op::A64InstructionSynchronizationBarrier:
        case Op::PushRSB: return;
        case Op::A64DataSynchronizationBarrier:
        case Op::A64DataMemoryBarrier: o << "Barrier();\n"; return;
        case Op::A64GetCNTFRQ: o << "u32 " << Def(in) << " = 19200000u;\n"; return;
        case Op::A64GetCNTPCT: o << "u64 " << Def(in) << " = Cntpct(s);\n"; return;
        case Op::A64GetCTR: o << "u32 " << Def(in) << " = 0x8444c004u;\n"; return;
        case Op::A64GetDCZID: o << "u32 " << Def(in) << " = 4u;\n"; return;
        case Op::A64GetTPIDR: o << "u64 " << Def(in) << " = s.tpidr;\n"; return;
        case Op::A64GetTPIDRRO: o << "u64 " << Def(in) << " = s.tpidrro;\n"; return;
        case Op::A64SetTPIDR: o << "s.tpidr = " << A(0) << ";\n"; return;
        case Op::A64ClearExclusive: o << "ClearExclusive(s);\n"; return;
        case Op::A64ReadMemory8: o << "u8 " << Def(in) << " = Rd<u8>(s, " << A(1) << ");\n"; return;
        case Op::A64ReadMemory16: o << "u16 " << Def(in) << " = Rd<u16>(s, " << A(1) << ");\n"; return;
        case Op::A64ReadMemory32: o << "u32 " << Def(in) << " = Rd<u32>(s, " << A(1) << ");\n"; return;
        case Op::A64ReadMemory64: o << "u64 " << Def(in) << " = Rd<u64>(s, " << A(1) << ");\n"; return;
        case Op::A64ReadMemory128: o << "V128 " << Def(in) << " = Rd128(s, " << A(1) << ");\n"; return;
        case Op::A64ExclusiveReadMemory8: o << "u8 " << Def(in) << " = ExRead8(s, " << A(1) << ");\n"; return;
        case Op::A64ExclusiveReadMemory16: o << "u16 " << Def(in) << " = ExRead16(s, " << A(1) << ");\n"; return;
        case Op::A64ExclusiveReadMemory32: o << "u32 " << Def(in) << " = ExRead32(s, " << A(1) << ");\n"; return;
        case Op::A64ExclusiveReadMemory64: o << "u64 " << Def(in) << " = ExRead64(s, " << A(1) << ");\n"; return;
        case Op::A64ExclusiveReadMemory128: o << "V128 " << Def(in) << " = ExRead128(s, " << A(1) << ");\n"; return;
        case Op::A64WriteMemory8:
        case Op::A64WriteMemory16:
        case Op::A64WriteMemory32:
        case Op::A64WriteMemory64: {
            static const char* ty[] = {"u8", "u16", "u32", "u64"};
            const char* t = ty[int(op) - int(Op::A64WriteMemory8)];
            if (ordered(3))
                o << "WrOrdered(s, " << A(1) << ", " << t << "(" << A(2) << "));\n";
            else
                o << "Wr<" << t << ">(s, " << A(1) << ", " << A(2) << ");\n";
            return;
        }
        case Op::A64WriteMemory128: o << "Wr128(s, " << A(1) << ", " << A(2) << ");\n"; return;
        case Op::A64ExclusiveWriteMemory8:
        case Op::A64ExclusiveWriteMemory16:
        case Op::A64ExclusiveWriteMemory32:
        case Op::A64ExclusiveWriteMemory64:
        case Op::A64ExclusiveWriteMemory128: {
            static const char* n[] = {"8", "16", "32", "64", "128"};
            o << "u32 " << Def(in) << " = ExWrite" << n[int(op) - int(Op::A64ExclusiveWriteMemory8)] << "(s, " << A(1)
              << ", " << A(2) << ");\n";
            return;
        }
        case Op::VectorTable: {
            std::string args;
            int n = 0;
            for (size_t k = 0; k < in.NumArgs(); k++) {
                if (in.GetArg(k).IsEmpty())
                    continue;
                if (in.GetArg(k).GetType() != IR::Type::U128)
                    throw std::runtime_error("64-bit table");
                args += (n++ ? ", " : "") + A(k);
            }
            o << "op::Tbl " << Def(in) << "{{" << args << "}, " << n << "};\n";
            return;
        }
        case Op::GetCarryFromOp: o << "bool " << Def(in) << " = " << A(0) << "f.c;\n"; return;
        case Op::GetOverflowFromOp: o << "bool " << Def(in) << " = " << A(0) << "f.v;\n"; return;
        case Op::GetNZCVFromOp: {
            // the parent may have been constant-folded into an immediate
            const IR::Value pv = in.GetArg(0);
            if (!pv.IsImmediate() && IsAddSub(pv.GetInst()->GetOpcode()))
                o << "u32 " << Def(in) << " = " << A(0) << "f.nzcv;\n";
            else
                o << "u32 " << Def(in) << " = NZOf(" << A(0) << ");\n";
            return;
        }
        default: break;
        }
        // Generic opcode -> op::Name(args)
        const std::string name = IR::GetNameOf(op);
        std::string args;
        const bool state_op = StateOps().count(name) != 0;
        if (name.rfind("ConditionalSelect", 0) == 0)
            args = NzR();
        else if (state_op)
            args = "s";
        for (size_t k = 0; k < in.NumArgs(); k++)
            args += (args.empty() ? "" : ", ") + A(k);
        if (IsAddSub(op) && in.HasAssociatedPseudoOperation()) {
            const std::string v = Def(in);
            o << "auto " << v << "f = op::" << name << "F(" << args << "); auto " << v << " = " << v << "f.r;\n";
        } else {
            o << "auto " << Def(in) << " = " << (state_op ? "" : "op::") << name << "(" << args << ");\n";
        }
    }

    void Jump(u64 t, int depth) {
        const std::string pad(depth * 2, ' ');
        Flush(pad);
        if (is_call && !sh.has_svc) {
            const u64 ret = sh.ret_loc;
            o << pad << "s.pc = " << Hex(t) << "; ";
            if (gen_index.count(t))
                o << Fn(t) << "(s);";
            else
                o << "Dispatch(s);";
            if (blockset.count(ret))
                o << " if (s.pc != " << Hex(ret) << ") return; goto b_" << std::hex << ret << std::dec << ";\n";
            else
                o << " return;\n";
            return;
        }
        if (blockset.count(t) && (t == f.entry || !g_entries.count(t))) {
            if (t <= cur)
                o << pad << "if (*s.halt) { s.pc = " << Hex(t) << "; return; }\n";
            o << pad << "goto b_" << std::hex << t << std::dec << ";\n";
            return;
        }
        o << pad << "s.pc = " << Hex(t) << "; ";
        if (gen_index.count(t))
            o << "return " << Fn(t) << "(s);\n";
        else
            o << "return;\n";
    }

    void Term(const IR::Terminal& t, int depth) {
        const std::string pad(depth * 2, ' ');
        if (auto* l = boost::get<IR::Term::LinkBlock>(&t)) {
            Jump(PcOf(l->next), depth);
        } else if (auto* l = boost::get<IR::Term::LinkBlockFast>(&t)) {
            Jump(PcOf(l->next), depth);
        } else if (auto* i = boost::get<IR::Term::If>(&t)) {
            o << pad << "if (CondHolds(" << NzR() << ", " << int(i->if_) << ")) {\n";
            Term(i->then_, depth + 1);
            o << pad << "} else {\n";
            Term(i->else_, depth + 1);
            o << pad << "}\n";
        } else if (auto* c = boost::get<IR::Term::CheckBit>(&t)) {
            o << pad << "if (cbit) {\n";
            Term(c->then_, depth + 1);
            o << pad << "} else {\n";
            Term(c->else_, depth + 1);
            o << pad << "}\n";
        } else if (auto* h = boost::get<IR::Term::CheckHalt>(&t)) {
            // Halts are checked right after the SVC and at back edges.
            Term(h->else_, depth);
        } else if (IsDynamic(t)) {
            if (sh.setpc_imm && !(is_call && !sh.has_svc)) {
                Jump(*sh.setpc_imm, depth);
            } else if (is_call) {
                Flush(pad);
                o << pad << "Dispatch(s);";
                if (blockset.count(sh.ret_loc))
                    o << " if (s.pc != " << Hex(sh.ret_loc) << ") return; goto b_" << std::hex << sh.ret_loc
                      << std::dec << ";\n";
                else
                    o << " return;\n";
            } else if (boost::get<IR::Term::PopRSBHint>(&t)) {
                Flush(pad);
                o << pad << "return;\n";
            } else {
                // Register branch: a local jump table target, a tail call or unknown code.
                Flush(pad);
                if (f.blocks.size() <= 4096) {
                    o << pad << "switch (s.pc) {\n";
                    for (u64 b : f.blocks)
                        o << pad << "case " << Hex(b) << ": goto b_" << std::hex << b << std::dec << ";\n";
                    o << pad << "default: break;\n" << pad << "}\n";
                }
                o << pad << "Dispatch(s); return;\n";
            }
        } else {
            Flush(pad);
            o << pad << "s.pc = " << Hex(cur) << "; return;\n";
        }
    }

    // Returns the number of guest instructions in the block (0 when it exits to the JIT immediately).
    void Block(u64 pc, const BlockInfo& bi) {
        o.str("");
        o.clear();
        try {
            BlockBody(pc, bi);
            out << o.str();
        } catch (const std::exception& e) {
            out << "b_" << std::hex << pc << std::dec << ": { s.pc = " << Hex(pc) << "; return; } // " << e.what()
                << " in " << cur_op << "\n";
            g_emit_failures++;
            g_blocks[pc].supported = false;
        }
    }
    void BlockBody(u64 pc, const BlockInfo& bi) {
        cur = pc;
        o << "b_" << std::hex << pc << std::dec << ": {\n";
        if (!bi.supported) {
            o << "  s.pc = " << Hex(pc) << "; return;\n}\n";
            return;
        }
        IR::Block b = TranslateAt(pc);
        sh = Shape(b);
        is_call = sh.has_push_rsb && !sh.has_svc;
        names.clear();
        used_regs.clear();
        dirty_regs.clear();
        sp_used = sp_dirty = nz_used = nz_dirty = false;
        const std::string head = o.str();
        o.str("");
        o.clear();
        o << "  s.icount += " << b.CycleCount() << ";\n";
        for (const auto& in : b) {
            o << "  ";
            Inst(in);
        }
        Term(b.GetTerminal(), 1);
        std::string body = o.str();
        for (size_t at; (at = body.find("@RELOAD@")) != std::string::npos;)
            body.replace(at, 8, LoadText(false));
        o.str("");
        o.clear();
        o << head << LoadText(true) << body << "}\n";
    }
};

// Output file that is only rewritten when its content changes, so a regeneration that touches one shard does not
// make the build recompile all of them.
struct ChangedFile : std::ostringstream {
    std::filesystem::path path;
    explicit ChangedFile(std::filesystem::path p) : path(std::move(p)) {}
    ~ChangedFile() {
        const std::string now = str();
        {
            std::ifstream in(path, std::ios::binary);
            const std::string old{std::istreambuf_iterator<char>(in), {}};
            if (in && old == now)
                return;
        }
        std::ofstream(path, std::ios::binary) << now;
    }
};

u64 Fnv(u64 h, u32 w) {
    for (int i = 0; i < 4; i++) {
        h ^= (w >> (i * 8)) & 0xFF;
        h *= 0x100000001b3ull;
    }
    return h;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 5) {
        std::printf("usage: recomp <out dir> <module.bin> <base> <text size> [<module.bin> <base> <text size>...] "
                    "[--profile f] [--coverage x] [--max-funcs n]\n");
        return 1;
    }
    const std::filesystem::path out = argv[1];
    std::vector<std::string> profiles;
    double coverage = 1.0;
    size_t max_funcs = SIZE_MAX;
    bool stats_only = false, all_funcs = false;
    std::set<u64> noopt_funcs, skip_funcs;
    int i = 2;
    for (; i + 2 < argc && argv[i][0] != '-'; i += 3) {
        Module m;
        std::ifstream in(argv[i], std::ios::binary);
        m.image.assign(std::istreambuf_iterator<char>(in), {});
        m.base = std::strtoull(argv[i + 1], nullptr, 16);
        m.text_end = m.base + std::strtoull(argv[i + 2], nullptr, 16);
        g_mods.push_back(std::move(m));
    }
    for (; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        if (k == "--profile")
            profiles.push_back(argv[i + 1]);
        else if (k == "--coverage")
            coverage = std::atof(argv[i + 1]);
        else if (k == "--all") // profiles only add entries; emit every function
            all_funcs = true;
        else if (k == "--noopt") { // file with function entries (hex) to compile without optimisation
            std::ifstream in(argv[i + 1]);
            for (std::string a; in >> a;)
                noopt_funcs.insert(std::strtoull(a.c_str(), nullptr, 16));
        } else if (k == "--skip") { // functions MSVC cannot compile at all: left to the JIT
            std::ifstream in(argv[i + 1]);
            for (std::string a; in >> a;)
                skip_funcs.insert(std::strtoull(a.c_str(), nullptr, 16));
        } else if (k == "--stats")
            stats_only = true;
        else if (k == "--max-funcs")
            max_funcs = std::strtoull(argv[i + 1], nullptr, 10);
    }
    std::filesystem::create_directories(out);

    for (const auto& m : g_mods) {
        g_image = m.image;
        g_base = m.base;
        g_text_end = m.text_end;
        FindEntries(m.text_end - m.base);
    }
    // Profiles (each normalised to the same total, so a sampled-PC profile and a JIT-exit profile weigh alike)
    std::unordered_map<u64, double> samples;
    double total_samples = 0;
    for (const auto& path : profiles) {
        std::unordered_map<u64, double> one;
        double sum = 0;
        std::ifstream in(path);
        std::string a;
        double n;
        while (in >> a >> n) {
            one[std::strtoull(a.c_str(), nullptr, 16)] += n;
            sum += n;
        }
        for (auto [pc, w] : one)
            samples[pc] += w * 1e6 / std::max(sum, 1.0);
        total_samples += 1e6;
    }
    std::vector<Function> funcs;
    for (u64 e : g_entries)
        funcs.push_back(Discover(e));
    // Profiled pcs no discovered function reaches (code only reached through computed pointers): new entries.
    {
        std::unordered_set<u64> inside;
        for (auto& f : funcs)
            for (u64 b : f.blocks)
                for (u64 a = b; a < g_blocks[b].end; a += 4)
                    inside.insert(a);
        size_t added = 0;
        for (auto [pc, w] : samples) {
            if (InText(pc) && !inside.count(pc) && g_entries.insert(pc).second) {
                funcs.push_back(Discover(pc));
                added++;
            }
        }
        std::printf("%zu entries added from uncovered profile pcs\n", added);
    }
    size_t nblocks = 0;
    for (auto& f : funcs)
        nblocks += f.blocks.size();
    std::printf("%zu functions, %zu blocks (%zu unique)\n", funcs.size(), nblocks, g_blocks.size());

    // Profile weighting: samples are block-start pcs; credit every function containing them.
    std::map<std::string, double> unsupported_weight;
    if (!samples.empty()) {
        // interval map of block ranges -> owning functions
        std::vector<std::pair<u64, size_t>> starts;
        for (size_t i = 0; i < funcs.size(); i++)
            for (u64 b : funcs[i].blocks)
                starts.emplace_back(b, i);
        std::sort(starts.begin(), starts.end());
        double covered = 0;
        for (auto [pc, cnt] : samples) {
            auto it = std::upper_bound(starts.begin(), starts.end(), std::make_pair(pc, SIZE_MAX));
            bool hit = false;
            std::set<size_t> owners;
            // walk back over blocks starting at or before pc whose range contains it
            for (auto j = it; j != starts.begin();) {
                --j;
                if (pc - j->first > 0x4000)
                    break;
                const BlockInfo& bi = g_blocks[j->first];
                if (pc < bi.end && owners.insert(j->second).second) {
                    funcs[j->second].weight += cnt;
                    hit = true;
                    if (j->first == pc && !bi.supported)
                        for (auto& op : bi.unsupported_ops)
                            unsupported_weight[op] += cnt / bi.unsupported_ops.size();
                }
            }
            if (hit)
                covered += cnt;
        }
        std::printf("profile: %.0f samples, %.1f%% inside discovered functions\n", total_samples,
                    100.0 * covered / std::max(total_samples, 1.0));
        std::sort(funcs.begin(), funcs.end(), [](auto& x, auto& y) { return x.weight > y.weight; });
        // keep the hottest functions until `coverage` of the (attributed) weight is reached
        double sum = 0, acc = 0;
        for (auto& f : funcs)
            sum += f.weight;
        size_t keep = 0;
        while (keep < funcs.size() && funcs[keep].weight > 0 && acc < coverage * sum) {
            acc += funcs[keep].weight;
            keep++;
        }
        if (!all_funcs)
            funcs.resize(keep);
    }
    if (stats_only) {
        // Static coverage over every discovered block, weighted by guest instruction count.
        std::map<std::string, u64> ops;
        u64 total = 0, bad = 0;
        for (auto& [pc, bi] : g_blocks) {
            const u64 n = (std::max(bi.end, pc + 4) - pc) / 4;
            total += n;
            if (bi.supported)
                continue;
            bad += n;
            for (auto& op : bi.unsupported_ops)
                ops[op] += n;
        }
        std::printf("static: %llu instructions in %zu blocks, %.2f%% in unsupported blocks\n", total, g_blocks.size(),
                    100.0 * double(bad) / double(total));
        std::vector<std::pair<u64, std::string>> v;
        for (auto& [k, n] : ops)
            v.emplace_back(n, k);
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 60; i++)
            std::printf("  %8llu %s\n", v[i].first, v[i].second.c_str());
        return 0;
    }
    if (funcs.size() > max_funcs)
        funcs.resize(max_funcs);
    // A function whose first block cannot run natively would bounce between the JIT trap and the native exit.
    std::erase_if(funcs, [](const Function& f) { return !g_blocks[f.entry].supported; });
    std::sort(funcs.begin(), funcs.end(), [](auto& x, auto& y) { return x.entry < y.entry; });

    std::unordered_map<u64, u32> gen_index;
    for (u32 i = 0; i < funcs.size(); i++)
        gen_index[funcs[i].entry] = i;

    // Shards hold the functions of one 256 KB guest code window, named after it: adding or changing a function
    // only rewrites its own shard (and the table).
    std::filesystem::remove(out / "recomp_decls.h");
    size_t shard = 0, emitted_blocks = 0, unsupported_blocks = 0;
    std::ostringstream cur;
    std::set<u64> called, defined;
    std::set<std::string> written;
    u64 cur_bucket = ~0ull;
    auto flush = [&] {
        if (cur.tellp() == 0)
            return;
        const std::string name = Emitter::fmt("recomp_%llx.cpp", cur_bucket);
        written.insert(name);
        shard++;
        ChangedFile f(out / name);
        f << "#include \"core/cpu/recomp/recomp_rt.h\"\n#pragma warning(disable : 4102 4189 4100)\nnamespace recomp {\n";
        for (u64 c : called) // including functions defined further down this shard
            f << "void f_" << std::hex << c << std::dec << "(State& s);\n";
        f << cur.str() << "}\n";
        cur.str("");
        cur.clear();
        called.clear();
        defined.clear();
    };
    std::vector<std::pair<u64, u64>> all_ranges;
    std::vector<std::tuple<u64, u32, u32, u64>> table; // entry, range_first, range_count, hash
    std::vector<std::pair<u64, u32>> resumes;           // pc, function index
    std::set<u64> disabled;                             // entry block failed to emit: not registered
    for (u32 idx = 0; idx < funcs.size(); idx++) {
        const Function& f = funcs[idx];
        if ((f.entry >> 18) != cur_bucket) {
            flush();
            cur_bucket = f.entry >> 18;
        }
        defined.insert(f.entry);
        cur << "u8 ok_" << std::hex << f.entry << std::dec << ";\n";
        std::unordered_set<u64> blockset(f.blocks.begin(), f.blocks.end());
        // Blocks first: a block that fails to emit becomes an exit stub, and must not be the entry or a resume point.
        std::ostringstream body;
        Emitter em{body, f, blockset, gen_index};
        em.called = &called;
        em.Block(f.entry, g_blocks[f.entry]);
        for (u64 b : f.blocks) {
            const BlockInfo& bi = g_blocks[b];
            emitted_blocks++;
            unsupported_blocks += !bi.supported;
            if (b != f.entry)
                em.Block(b, bi);
        }
        // MSVC cannot compile a handful of functions even unoptimised: an empty stub (exits at once, so the JIT runs
        // it). Keeping the slot keeps every other function's index, so only this shard is rebuilt.
        const bool skip = skip_funcs.count(f.entry) != 0;
        if (!g_blocks[f.entry].supported || skip)
            disabled.insert(f.entry);
        if (skip)
            cur << "void f_" << std::hex << f.entry << std::dec << "(State& s) {}\n\n";
        // MSVC's optimizer crashes (internal compiler error) on a few functions: build those unoptimised.
        const bool noopt = !skip && noopt_funcs.count(f.entry) != 0;
        if (skip) {
        } else if (auto in = g_intrinsics.find(f.entry); in != g_intrinsics.end()) {
            cur << "void f_" << std::hex << f.entry << std::dec << "(State& s) {\n  if (!ok_" << std::hex << f.entry
                << std::dec << ") return;\n  Libc_" << in->second << "(s);\n  s.pc = s.x[30];\n}\n\n";
        } else {
        if (noopt)
            cur << "#pragma optimize(\"\", off)\n";
        cur << "void f_" << std::hex << f.entry << std::dec << "(State& s) {\n  if (!ok_" << std::hex << f.entry << std::dec
            << " || reinterpret_cast<u64>(_AddressOfReturnAddress()) < s.stack_limit) return;\n"
               "  bool cbit = false;\n";
        // Resume points: call-return blocks (the JIT can come back into this function after a callee it ran) and the
        // blocks after SVCs (a thread halted inside an SVC continues there).
        std::vector<u64> resume;
        for (u64 b : f.blocks) {
            for (const u64 r : {g_blocks[b].ret, g_blocks[b].svc_next})
                if (r && r != f.entry && blockset.count(r) && g_blocks[r].supported)
                    resume.push_back(r);
        }
        std::sort(resume.begin(), resume.end());
        resume.erase(std::unique(resume.begin(), resume.end()), resume.end());
        if (!resume.empty()) {
            cur << "  if (s.pc != 0x" << std::hex << f.entry << "ull) switch (s.pc) {\n";
            for (u64 r : resume) {
                cur << "  case 0x" << r << "ull: goto b_" << r << ";\n";
                resumes.emplace_back(r, idx);
            }
            cur << std::dec << "  default: return;\n  }\n";
        }
        cur << body.str() << "}\n\n";
        if (noopt)
            cur << "#pragma optimize(\"\", on)\n";
        }
        // verification ranges: merged [start, end) of all blocks
        std::vector<std::pair<u64, u64>> r;
        for (u64 b : f.blocks)
            r.emplace_back(b, std::max(g_blocks[b].end, b + 4));
        std::sort(r.begin(), r.end());
        std::vector<std::pair<u64, u64>> m;
        for (auto& x : r) {
            if (!m.empty() && x.first <= m.back().second)
                m.back().second = std::max(m.back().second, x.second);
            else
                m.push_back(x);
        }
        u64 h = 0xcbf29ce484222325ull;
        for (auto& x : m)
            for (u64 a = x.first; a < x.second && InText(a); a += 4)
                h = Fnv(h, Word(a));
        table.emplace_back(f.entry, u32(all_ranges.size()), u32(m.size()), h);
        all_ranges.insert(all_ranges.end(), m.begin(), m.end());
    }
    flush();
    for (const auto& e : std::filesystem::directory_iterator(out)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("recomp_", 0) == 0 && n.ends_with(".cpp") && n != "recomp_table.cpp" && !written.count(n))
            std::filesystem::remove(e.path());
    }
    {
        ChangedFile t(out / "recomp_table.cpp");
        t << "#include \"core/cpu/recomp/recomp_rt.h\"\nnamespace recomp {\n";
        for (auto& f : funcs)
            t << "void f_" << std::hex << f.entry << "(State& s);\nextern u8 ok_" << f.entry << ";\n" << std::dec;
        t << "static u8* const kOk[] = {\n";
        for (auto& f : funcs)
            t << "&ok_" << std::hex << f.entry << ",\n" << std::dec;
        if (funcs.empty())
            t << "nullptr,\n";
        t << "};\nstatic const FuncEntry kFuncs[] = {\n";
        for (auto& [e, rf, rc, h] : table)
            t << "{0x" << std::hex << e << "ull, " << (disabled.count(e) ? std::string("nullptr") : "f_" + Emitter::fmt("%llx", e))
              << ", " << std::dec << rf << ", " << rc << ", 0x" << std::hex
              << h << "ull},\n"
              << std::dec;
        if (table.empty())
            t << "{0, nullptr, 0, 0, 0},\n";
        t << "};\nstatic const CodeRange kRanges[] = {\n";
        for (auto& [a, b] : all_ranges)
            t << "{0x" << std::hex << a << "ull, 0x" << b << "ull},\n" << std::dec;
        if (all_ranges.empty())
            t << "{0, 0},\n";
        t << "};\nstatic const ResumeEntry kResumes[] = {\n";
        for (auto& [pc, fi] : resumes)
            t << "{0x" << std::hex << pc << "ull, " << std::dec << fi << "},\n";
        if (resumes.empty())
            t << "{0, 0},\n";
        t << "};\nstatic const Table kTable{kFuncs, " << table.size() << ", kRanges, " << all_ranges.size()
          << ", kOk, kResumes, " << resumes.size() << "};\nconst Table& GetTable() { return kTable; }\n}\n";
    }
    std::printf("%zu blocks failed to emit\n", g_emit_failures);
    std::printf("%zu C library routines run on the host\n", g_intrinsics.size());
    std::printf("%zu resume points\n", resumes.size());
    std::printf("%zu jump tables resolved (%zu targets)\n", g_jump_tables, g_jump_targets);
    std::printf("emitted %zu functions, %zu blocks (%zu exit to the JIT) in %zu shards\n", funcs.size(),
                emitted_blocks, unsupported_blocks, shard);
    if (!unsupported_weight.empty()) {
        std::vector<std::pair<double, std::string>> v;
        for (auto& [k, w] : unsupported_weight)
            v.emplace_back(w, k);
        std::sort(v.rbegin(), v.rend());
        std::printf("unsupported opcodes by profile weight (%% of samples):\n");
        for (size_t i = 0; i < v.size() && i < 40; i++)
            std::printf("  %6.2f%% %s\n", 100.0 * v[i].first / total_samples, v[i].second.c_str());
    }
    return 0;
}
