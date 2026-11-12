#include <array>
#include <set>
#include <cstdlib>
// Maxwell SASS -> HLSL (Shader Model 5.0) translator.
#include "gpu/shader/translator.h"

#include <algorithm>
#include <functional>
#include <map>

#include "gpu/shader/maxwell_isa.h"

namespace gpu::shader {

namespace {

constexpr u32 kProgramStart = 0x50; // instructions follow the 0x50-byte SPH
// Generic varyings passed VS->PS (D3D11 PS input limit is 32 incl. position and front-face).
constexpr u32 kMaxVaryings = 30;

u32 NextPc(u32 pc) {
    pc += 8;
    if (pc % 32 == 0)
        pc += 8; // skip scheduling word
    return pc;
}

s32 BranchOffset(u64 insn) { return s32(u32(Bits(insn, 20, 24)) << 8) >> 8; }
// Branch/SSY/PBK/PCNT targets are relative to the raw following word (pc + 8), even when that word is a
// scheduling control word; a target that lands on a control word means the instruction after it.
u32 BranchTarget(u32 pc, u64 insn) {
    u32 t = u32(s32(pc + 8) + BranchOffset(insn));
    if (t % 32 == 0)
        t += 8;
    return t;
}

std::string Hex(u32 v) { return fmt::format("0x{:X}u", v); }

struct Block {
    u32 start = 0, end = 0; // [start, end) in program bytes
    u32 id = 0;
};

class Translator {
public:
    Translator(Stage stage, const u8* code, size_t size, const TranslateOptions& opt, TranslateResult& res)
        : stage_(stage), code_(code), size_(size), opt_(opt), res_(res) {
        std::memcpy(sph_, code, std::min<size_t>(size, 0x50));
    }

    bool Run();

private:
    // ---------------------------------------------------------------- program access
    bool Valid(u32 pc) const { return kProgramStart + pc + 8 <= size_; }
    u64 At(u32 pc) const {
        u64 v;
        std::memcpy(&v, code_ + kProgramStart + pc, 8);
        return v;
    }

    // ---------------------------------------------------------------- emission helpers
    void Line(const std::string& s) {
        out_.append(size_t(indent_) * 4, ' ');
        out_ += s;
        out_ += '\n';
    }
    void Unimpl(Op op, u32 pc, u64 insn) {
        res_.unimplemented.insert(OpName(op));
        Line(fmt::format("// UNIMPLEMENTED {} at {:#x} ({:016x})", OpName(op), pc, insn));
    }

    std::string R(u32 i) {
        if (i == RZ)
            return "0u";
        regs_used_.insert(i);
        return fmt::format("r{}", i);
    }
    std::string F(u32 i) { return i == RZ ? "0.0f" : fmt::format("asfloat({})", R(i)); }
    void SetR(u32 i, const std::string& v) {
        if (i == RZ)
            return;
        Line(fmt::format("{} = {};", R(i), v));
    }
    void SetF(u32 i, const std::string& v) { SetR(i, fmt::format("asuint({})", v)); }
    std::string P(u32 i, bool neg) {
        std::string s = i == PT ? "true" : fmt::format("p{}", i);
        if (i != PT)
            preds_used_.insert(i);
        return neg ? "(!" + s + ")" : s;
    }
    void SetP(u32 i, const std::string& v) {
        if (i == PT)
            return;
        preds_used_.insert(i);
        Line(fmt::format("p{} = {};", i, v));
    }
    std::string Cbuf(u32 index, u32 byte_off) {
        res_.cbuf_mask |= 1u << index;
        if (index < 18)
            res_.cbuf_rows[index] = cbuf_rows_[index] = std::max(cbuf_rows_[index], (byte_off >> 4) + 1);
        static const char* comp = "xyzw";
        return fmt::format("c{}[{}].{}", index, byte_off >> 4, comp[(byte_off >> 2) & 3]);
    }
    std::string CbufDyn(u32 index, const std::string& byte_addr) {
        res_.cbuf_mask |= 1u << index;
        res_.cbuf_dyn_mask |= 1u << index;
        return fmt::format("CbufLoad{}({})", index, byte_addr);
    }

    // Operand forms
    std::string CbufOperand(u64 insn) { return Cbuf(u32(Bits(insn, 34, 5)), u32(Bits(insn, 20, 14)) * 4); }
    std::string IntImm20(u64 insn) {
        s32 v = s32(Bits(insn, 20, 19));
        if (Bit(insn, 56))
            v -= 1 << 19;
        return Hex(u32(v));
    }
    std::string FloatImm20(u64 insn) {
        const u32 v = (u32(Bits(insn, 20, 19)) << 12) | (u32(Bit(insn, 56)) << 31);
        return fmt::format("asfloat({})", Hex(v));
    }
    std::string Imm32(u64 insn) { return Hex(u32(Bits(insn, 20, 32))); }

    // B operand for the reg/cbuf/imm triple, as uint.
    std::string SrcB(u64 insn, int form /*0 reg,1 cbuf,2 imm*/) {
        switch (form) {
        case 0: return R(u32(Bits(insn, 20, 8)));
        case 1: return CbufOperand(insn);
        default: return IntImm20(insn);
        }
    }
    std::string SrcBF(u64 insn, int form) {
        switch (form) {
        case 0: return F(u32(Bits(insn, 20, 8)));
        case 1: return "asfloat(" + CbufOperand(insn) + ")";
        default: return FloatImm20(insn);
        }
    }

    static std::string FAbsNeg(std::string v, bool abs, bool neg) {
        if (abs)
            v = "abs(" + v + ")";
        if (neg)
            v = "(-" + v + ")";
        return v;
    }
    static std::string Sat(const std::string& v, bool sat) { return sat ? "saturate(" + v + ")" : v; }

    std::string FCompare(u32 op, const std::string& a, const std::string& b) {
        switch (op) {
        case 0: return "false";
        case 1: return fmt::format("({} < {})", a, b);
        case 2: return fmt::format("({} == {})", a, b);
        case 3: return fmt::format("({} <= {})", a, b);
        case 4: return fmt::format("({} > {})", a, b);
        case 5: return fmt::format("({} != {} && !isnan({}) && !isnan({}))", a, b, a, b);
        case 6: return fmt::format("({} >= {})", a, b);
        case 7: return fmt::format("(!isnan({}) && !isnan({}))", a, b);
        case 8: return fmt::format("(isnan({}) || isnan({}))", a, b);
        case 9: return fmt::format("(!({} >= {}))", a, b);
        case 10: return fmt::format("(!({} != {}))", a, b);
        case 11: return fmt::format("(!({} > {}))", a, b);
        case 12: return fmt::format("(!({} <= {}))", a, b);
        case 13: return fmt::format("({} != {})", a, b);
        case 14: return fmt::format("(!({} < {}))", a, b);
        default: return "true";
        }
    }
    std::string ICompare(u32 op, bool is_signed, const std::string& a, const std::string& b) {
        const std::string x = is_signed ? "int(" + a + ")" : a, y = is_signed ? "int(" + b + ")" : b;
        switch (op) {
        case 0: return "false";
        case 1: return fmt::format("({} < {})", x, y);
        case 2: return fmt::format("({} == {})", x, y);
        case 3: return fmt::format("({} <= {})", x, y);
        case 4: return fmt::format("({} > {})", x, y);
        case 5: return fmt::format("({} != {})", x, y);
        case 6: return fmt::format("({} >= {})", x, y);
        default: return "true";
        }
    }
    static std::string Bop(u32 op, const std::string& a, const std::string& b) {
        switch (op) {
        case 0: return fmt::format("({} && {})", a, b);
        case 1: return fmt::format("({} || {})", a, b);
        default: return fmt::format("({} != {})", a, b);
        }
    }
    std::string CcTest(u32 t) {
        cc_used_ = true;
        switch (t) {
        case 0: return "false";
        case 1: return "(cc_s != cc_o)";
        case 2: return "cc_z";
        case 3: return "(cc_z || cc_s != cc_o)";
        case 4: return "(!cc_z && cc_s == cc_o)";
        case 5: return "(!cc_z)";
        case 6: return "(cc_s == cc_o)";
        case 7: return "(!(cc_z && cc_s))";
        case 8: return "(cc_z && cc_s)";
        case 9: return "(cc_s != cc_o || (cc_z && cc_s))";
        case 10: return "(cc_z || (cc_z && cc_s))";
        case 11: return "(cc_z || cc_s != cc_o)";
        case 12: return "(!cc_z && cc_s == cc_o)";
        case 13: return "(!cc_z || (cc_z && cc_s))";
        case 14: return "(cc_s == cc_o || (cc_z && cc_s))";
        case 15: return "true";
        case 16: return "(!cc_o)";
        case 17: return "(!cc_c)";
        case 18: return "(!cc_s)";
        case 19: return "(!cc_c || cc_z)";
        case 20: return "(cc_c && !cc_z)";
        case 21: return "cc_s";
        case 22: return "cc_c";
        case 23: return "cc_o";
        default: return "true";
        }
    }
    void SetCcInt(const std::string& v) {
        cc_used_ = true;
        Line(fmt::format("cc_z = ({}) == 0u; cc_s = int({}) < 0;", v, v));
    }
    void SetCcFloat(const std::string& v) {
        cc_used_ = true;
        Line(fmt::format("cc_z = asfloat({}) == 0.0f; cc_s = asfloat({}) < 0.0f; cc_c = false; cc_o = false;", v, v));
    }

    // ---------------------------------------------------------------- attributes
    std::string AttrRead(u32 addr, const std::string& index_expr = "");
    void AttrWrite(u32 addr, const std::string& value);

    // ---------------------------------------------------------------- textures
    u32 TexSlot(u32 cbuf_off, TexDim dim, bool array, bool shadow);
    std::string Sampler(u32 slot) { return fmt::format("smp{}", slot); }
    u32 cbuf_rows_[18]{}; // highest statically referenced row + 1, per constant buffer
    std::string CmpSampler(u32 slot) { return fmt::format("smpc{}", slot); }
    std::set<u32> declared_smp_, declared_smpc_;
    void EmitTexWrites(u32 mask, const std::vector<u32>& regs, const std::string& value, u32 slot);
    // TEXS/TLDS/TLD4S without the FP32 flag: the selected components are packed as half2, two per register.
    void EmitTexWritesF16(u32 mask, u32 d0, u32 d1, const std::string& value, u32 slot);
    void TexSample(u64 insn, u32 pc, Op op);

    // ---------------------------------------------------------------- flow
    bool DiscoverBlocks();
    bool AnalyzeStacks();

    // ---------------------------------------------------------------- structured emission
    // Block bodies plus their terminators, re-emitted as nested if/else and natural loops (FXC handles these
    // well; the generic block-dispatch loop can take it minutes). Returns false for shapes it does not model.
    struct Term {
        enum Kind { Fall, Jump, Cond, Exit, CondExit, Switch } kind = Fall;
        std::string sel;                               // Switch: selector register
        std::vector<std::pair<u32, u32>> cases;        // Switch: (selector value, block id)
        std::string cond;      // Cond / CondExit
        u32 target = ~0u;      // block id, or ~0u for "outside the program"
        std::string exit_code; // Exit / CondExit
    };
    struct SBlock {
        std::string body;
        Term term;
    };
    std::vector<SBlock> sblocks_;
    std::string sout_;
    int sindent_ = 1;
    struct LoopCtx {
        u32 header = ~0u, exit = ~0u;
    };
    bool BuildStructuredBlocks();
    bool EmitStructured(std::string& body);
    std::string sfail_;
    // BRX (indirect branch) jump tables, resolved from constant buffer 1: pc -> (table value, target pc).
    std::map<u32, std::vector<std::pair<u32, u32>>> brx_;
    bool ResolveBrx(u32 pc, u64 insn);
    // Forward jumps out of a region's last block: the condition is kept in a flag and the blocks it skips
    // are wrapped in if (!flag) by the enclosing sequence.
    std::set<u32> pending_fwd_, fwd_used_;
    bool SFail(std::string why) {
        if (sfail_.empty())
            sfail_ = std::move(why);
        return false;
    }
    bool Seq(u32 first, u32 last, u32 end_target, LoopCtx ctx, u32 skip_header);
    bool Goto(u32 from, u32 target, u32 last, u32 end_target, LoopCtx ctx);
    void SLine(const std::string& l) { sout_.append(size_t(sindent_) * 4, ' ').append(l).append("\n"); }
    void SText(const std::string& text);
    void EmitInstruction(u32 pc, u64 insn, Op op);
    void EmitJump(u32 target);
    void EmitExit();
    void EmitPrologue(std::string& head);

    Stage stage_;
    const u8* code_;
    size_t size_;
    TranslateOptions opt_;
    TranslateResult& res_;
    u32 sph_[20]{};

    std::map<u32, Block> blocks_;      // by start pc
    std::map<u32, u32> block_of_;      // pc -> block start
    std::map<u32, u32> sync_target_;   // pc of SYNC/BRK/CONT -> resolved target
    std::map<u32, std::vector<std::pair<char, u32>>> block_stack_;
    bool has_back_edge_ = false;
    u32 cur_block_ = 0;

    std::string out_;
    int indent_ = 1;
    std::set<u32> regs_used_, preds_used_;
    bool cc_used_ = false;
    std::map<u32, u32> tex_slot_of_; // cbuf offset -> slot
    std::set<u32> vs_inputs_, out_attrs_, in_attrs_;
    bool uses_lmem_ = false, uses_smem_ = false, uses_gmem_ = false, uses_frag_coord_ = false, uses_front_face_ = false;
    bool gmem_written_ = false;
    // Atomic op (Maxwell order: ADD MIN MAX INC DEC AND OR XOR EXCH) on `dst`; `orig` receives the old value.
    std::string AtomicOp(u32 aop, const std::string& dst, const std::string& v, const std::string& orig) {
        switch (aop) {
        case 0: return fmt::format("InterlockedAdd({}, {}, {});", dst, v, orig);
        case 1: return fmt::format("InterlockedMin({}, {}, {});", dst, v, orig);
        case 2: return fmt::format("InterlockedMax({}, {}, {});", dst, v, orig);
        case 5: return fmt::format("InterlockedAnd({}, {}, {});", dst, v, orig);
        case 6: return fmt::format("InterlockedOr({}, {}, {});", dst, v, orig);
        case 7: return fmt::format("InterlockedXor({}, {}, {});", dst, v, orig);
        case 8: return fmt::format("InterlockedExchange({}, {}, {});", dst, v, orig);
        default:
            res_.unimplemented.insert(fmt::format("ATOM_op{}", aop));
            return fmt::format("InterlockedAdd({}, {}, {});", dst, v, orig);
        }
    }
    u32 SmemWords() const { return std::max<u32>(opt_.shared_bytes / 4, 1); }
    bool uses_vertex_id_ = false, uses_instance_id_ = false;
};

// ================================================================ attributes
std::string Translator::AttrRead(u32 addr, const std::string& index_expr) {
    const u32 comp = (addr >> 2) & 3;
    static const char* cs = "xyzw";
    if (addr >= 0x80 && addr < 0x280) {
        const u32 attr = (addr - 0x80) >> 4;
        if (stage_ == Stage::Vertex) {
            vs_inputs_.insert(attr);
            res_.attrib_in_mask |= 1u << attr;
            return fmt::format("vin{}.{}", attr, cs[comp]);
        }
        in_attrs_.insert(attr);
        return fmt::format("asuint(in_attr{}.{})", attr, cs[comp]);
    }
    if (addr >= 0x70 && addr < 0x80) {
        if (stage_ == Stage::Fragment) {
            uses_frag_coord_ = true;
            return fmt::format("asuint(frag_coord.{})", cs[comp]);
        }
        return fmt::format("asuint(o_pos.{})", cs[comp]);
    }
    switch (addr) {
    case 0x2F8: uses_instance_id_ = true; return "instance_id";
    case 0x2FC: uses_vertex_id_ = true; return "vertex_id";
    case 0x3FC: uses_front_face_ = true; return "(front_face ? 0xFFFFFFFFu : 0u)";
    case 0x2E0: case 0x2E4: return "0u"; // point coord
    case 0x6C: return "asuint(1.0f)";
    default:
        res_.unimplemented.insert(fmt::format("attr_read_{:#x}", addr));
        return "0u";
    }
}

void Translator::AttrWrite(u32 addr, const std::string& value) {
    const u32 comp = (addr >> 2) & 3;
    static const char* cs = "xyzw";
    if (addr >= 0x80 && addr < 0x27C + 4) {
        const u32 attr = (addr - 0x80) >> 4;
        if (attr < kMaxVaryings) {
            out_attrs_.insert(attr);
            Line(fmt::format("o_attr{}.{} = asfloat({});", attr, cs[comp], value));
        }
        return;
    }
    if (addr >= 0x70 && addr < 0x80) {
        Line(fmt::format("o_pos.{} = asfloat({});", cs[comp], value));
        return;
    }
    // point size, layer, viewport index, clip distances: ignored for now
}

// ================================================================ textures
u32 Translator::TexSlot(u32 cbuf_off, TexDim dim, bool array, bool shadow) {
    // Distinct slots per (handle, dimensionality): the same handle may be read as different view types.
    const u32 key = cbuf_off | (u32(dim) << 24) | (u32(array) << 28);
    auto it = tex_slot_of_.find(key);
    if (it != tex_slot_of_.end()) {
        auto& t = res_.textures[it->second];
        t.shadow |= shadow;
        return it->second;
    }
    TextureUse t;
    t.cbuf_offset = cbuf_off;
    t.dim = dim;
    t.array = array;
    t.shadow = shadow;
    t.slot = u32(res_.textures.size());
    res_.textures.push_back(t);
    tex_slot_of_[key] = t.slot;
    return t.slot;
}

void Translator::EmitTexWrites(u32 mask, const std::vector<u32>& regs, const std::string& value, u32 slot) {
    Line(fmt::format("tmp4 = ApplySwizzle({}, tex_swizzle[{}].x);", value, slot));
    static const char* cs = "xyzw";
    size_t r = 0;
    for (u32 c = 0; c < 4; c++) {
        if (!(mask & (1u << c)))
            continue;
        if (r < regs.size() && regs[r] != RZ)
            SetF(regs[r], fmt::format("tmp4.{}", cs[c]));
        r++;
    }
}

void Translator::EmitTexWritesF16(u32 mask, u32 d0, u32 d1, const std::string& value, u32 slot) {
    Line(fmt::format("tmp4 = {};", slot == ~0u ? value : fmt::format("ApplySwizzle({}, tex_swizzle[{}].x)", value, slot)));
    static const char* cs = "xyzw";
    std::vector<std::string> comps;
    for (u32 c = 0; c < 4; c++)
        if (mask & (1u << c))
            comps.push_back(fmt::format("tmp4.{}", cs[c]));
    auto pair = [&](size_t i) {
        return fmt::format("P2(float2({}, {}))", i < comps.size() ? comps[i] : "0.0f",
                           i + 1 < comps.size() ? comps[i + 1] : "0.0f");
    };
    if (!comps.empty())
        SetR(d0, pair(0));
    if (comps.size() > 2)
        SetR(d1, pair(2));
}

void Translator::TexSample(u64 insn, u32 pc, Op op) {
    const bool vs = stage_ != Stage::Fragment;
    auto lodsuffix = [&](const std::string& normal) { return (vs || opt_.force_lod0) ? std::string() : normal; };
    (void)lodsuffix;
    const u32 ra = u32(Bits(insn, 8, 8)), rb = u32(Bits(insn, 20, 8));
    const u32 cbuf_off = u32(Bits(insn, 36, 13)) * 4;
    auto coord = [&](u32 reg) { return F(reg); };
    auto sample = [&](u32 slot, const std::string& coords, int lod_kind, const std::string& lod,
                      const std::string& dref) -> std::string {
        // lod_kind: 0 implicit, 1 zero, 2 explicit lod, 3 bias
        const std::string t = fmt::format("tex{}", slot);
        if (!dref.empty()) {
            if (lod_kind == 0 && !vs && !opt_.force_lod0)
                return fmt::format("{}.SampleCmp({}, {}, {}).xxxx", t, CmpSampler(slot), coords, dref);
            return fmt::format("{}.SampleCmpLevelZero({}, {}, {}).xxxx", t, CmpSampler(slot), coords, dref);
        }
        if (lod_kind == 1 || ((vs || opt_.force_lod0) && lod_kind != 2))
            return fmt::format("{}.SampleLevel({}, {}, 0.0f)", t, Sampler(slot), coords);
        if (lod_kind == 2)
            return fmt::format("{}.SampleLevel({}, {}, {})", t, Sampler(slot), coords, lod);
        if (lod_kind == 3)
            return fmt::format("{}.SampleBias({}, {}, {})", t, Sampler(slot), coords, lod);
        return fmt::format("{}.Sample({}, {})", t, Sampler(slot), coords);
    };

    switch (op) {
    case Op::TEXS: {
        const u32 d0 = u32(Bits(insn, 0, 8)), d1 = u32(Bits(insn, 28, 8));
        const u32 enc = u32(Bits(insn, 53, 4));
        const u32 cmask = u32(Bits(insn, 50, 3));
        static const u32 lut[4][8] = {{0, 0, 0, 0, 0, 0, 0, 0},
                                      {0x1, 0x2, 0x4, 0x8, 0x3, 0x9, 0xa, 0xc},
                                      {0x1, 0x2, 0x4, 0x8, 0x3, 0x9, 0xa, 0xc},
                                      {0x7, 0xb, 0xd, 0xe, 0xf, 0xf, 0xf, 0xf}};
        const u32 mask = lut[(d1 != RZ ? 2 : 0) | (d0 != RZ ? 1 : 0)][cmask];
        std::string coords, lod, dref;
        int lk = 0;
        TexDim dim = TexDim::Tex2D;
        bool array = false, shadow = false;
        switch (enc) {
        case 0: dim = TexDim::Tex1D; coords = coord(ra); lk = 1; break;
        case 1: coords = fmt::format("float2({}, {})", coord(ra), coord(rb)); break;
        case 2: coords = fmt::format("float2({}, {})", coord(ra), coord(rb)); lk = 1; break;
        case 3: coords = fmt::format("float2({}, {})", coord(ra), coord(ra + 1)); lk = 2; lod = coord(rb); break;
        case 4: coords = fmt::format("float2({}, {})", coord(ra), coord(ra + 1)); shadow = true; dref = coord(rb); break;
        case 5: coords = fmt::format("float2({}, {})", coord(ra), coord(ra + 1)); lk = 2; lod = coord(rb);
            shadow = true; dref = coord(rb + 1); break;
        case 6: coords = fmt::format("float2({}, {})", coord(ra), coord(ra + 1)); lk = 1; shadow = true;
            dref = coord(rb); break;
        case 7: array = true; coords = fmt::format("float3({}, {}, float({}))", coord(ra + 1), coord(rb), R(ra)); break;
        case 8: array = true; lk = 1; coords = fmt::format("float3({}, {}, float({}))", coord(ra + 1), coord(rb), R(ra)); break;
        case 9: array = true; lk = 1; shadow = true;
            coords = fmt::format("float3({}, {}, float({}))", coord(ra + 1), coord(rb), R(ra)); dref = coord(rb + 1); break;
        case 10: dim = TexDim::Tex3D; coords = fmt::format("float3({}, {}, {})", coord(ra), coord(ra + 1), coord(rb)); break;
        case 11: dim = TexDim::Tex3D; lk = 1; coords = fmt::format("float3({}, {}, {})", coord(ra), coord(ra + 1), coord(rb)); break;
        case 12: dim = TexDim::Cube; coords = fmt::format("float3({}, {}, {})", coord(ra), coord(ra + 1), coord(rb)); break;
        case 13: dim = TexDim::Cube; lk = 2; lod = coord(rb + 1);
            coords = fmt::format("float3({}, {}, {})", coord(ra), coord(ra + 1), coord(rb)); break;
        default: Unimpl(op, pc, insn); return;
        }
        const u32 slot = TexSlot(cbuf_off, dim, array, shadow);
        if (!Bit(insn, 59)) // bit 59 clear: half-precision results packed in pairs
            EmitTexWritesF16(mask, d0, d1, sample(slot, coords, lk, lod, dref), slot);
        else
            EmitTexWrites(mask, {d0, d0 == RZ ? RZ : d0 + 1, d1, d1 == RZ ? RZ : d1 + 1},
                          sample(slot, coords, lk, lod, dref), slot);
        return;
    }
    case Op::TEX:
    case Op::TEX_b: {
        const u32 d = u32(Bits(insn, 0, 8));
        // 3-bit texture type: 1D, 1D array, 2D, 2D array, 3D, 3D array, cube, cube array
        const u32 type3 = u32(Bits(insn, 28, 3));
        const u32 type = type3 >> 1;
        const bool array = type3 & 1;
        const u32 mask = u32(Bits(insn, 31, 4));
        const bool dc = Bit(insn, 50), aoffi = Bit(insn, 54);
        const u32 blod = u32(Bits(insn, 55, 3));
        const TexDim dim = TexDim(type);
        u32 c = ra;
        std::string array_idx;
        if (array)
            array_idx = fmt::format("float({})", R(c++));
        const u32 ncoord = dim == TexDim::Tex1D ? 1 : dim == TexDim::Tex2D ? 2 : 3;
        std::string coords;
        for (u32 i = 0; i < ncoord; i++)
            coords += (i ? ", " : "") + coord(c + i);
        if (array)
            coords += ", " + array_idx;
        const u32 n = ncoord + (array ? 1 : 0);
        coords = n == 1 ? coords : fmt::format("float{}({})", n, coords);
        u32 meta = rb;
        int lk = 0;
        std::string lod, dref;
        if (blod == 1) lk = 1;
        else if (blod == 2 || blod == 6) { lk = 3; lod = coord(meta++); }
        else if (blod == 3 || blod == 7) { lk = 2; lod = coord(meta++); }
        if (aoffi)
            meta++; // offsets ignored for now
        if (dc)
            dref = coord(meta++);
        const u32 slot = TexSlot(cbuf_off, dim, array, dc);
        std::vector<u32> regs;
        for (u32 i = 0; i < 4; i++)
            regs.push_back(d == RZ ? RZ : d + i);
        EmitTexWrites(mask, regs, sample(slot, coords, lk, lod, dref), slot);
        return;
    }
    case Op::TLDS: {
        const u32 d0 = u32(Bits(insn, 0, 8)), d1 = u32(Bits(insn, 28, 8));
        const u32 enc = u32(Bits(insn, 53, 4));
        const u32 cmask = u32(Bits(insn, 50, 3));
        static const u32 lut[4][8] = {{0, 0, 0, 0, 0, 0, 0, 0},
                                      {0x1, 0x2, 0x4, 0x8, 0x3, 0x9, 0xa, 0xc},
                                      {0x1, 0x2, 0x4, 0x8, 0x3, 0x9, 0xa, 0xc},
                                      {0x7, 0xb, 0xd, 0xe, 0xf, 0xf, 0xf, 0xf}};
        const u32 mask = lut[(d1 != RZ ? 2 : 0) | (d0 != RZ ? 1 : 0)][cmask];
        std::string load;
        TexDim dim = TexDim::Tex2D;
        bool array = false;
        u32 slot;
        switch (enc) {
        case 0: // 1D, LOD 0
        case 1: // 1D, explicit LOD in rb (texel buffers have one level)
            dim = TexDim::Buffer; slot = TexSlot(cbuf_off, dim, false, false);
            load = fmt::format("tex{}.Load(int({}))", slot, R(ra)); break;
        case 2: // 2D, LOD 0, x in ra and y in rb (shaders put them in neighbouring registers)
            slot = TexSlot(cbuf_off, dim, false, false);
            load = fmt::format("tex{0}.Load(int3(RsXY(int2(int({1}), int({2})), {0}u), 0))", slot, R(ra), R(rb)); break;
        case 4: slot = TexSlot(cbuf_off, dim, false, false);
            load = fmt::format("tex{0}.Load(int3(RsXY(int2(int({1}), int({2})), {0}u), 0))", slot, R(ra), R(ra + 1)); break;
        case 8: slot = TexSlot(cbuf_off, dim, false, false);
            load = fmt::format("tex{0}.Load(int3(RsXY(int2(int({1}), int({2})), {0}u), 0))", slot, R(ra), R(ra + 1)); break;
        case 10: dim = TexDim::Tex3D; slot = TexSlot(cbuf_off, dim, false, false);
            load = fmt::format("tex{}.Load(int4(int({}), int({}), int({}), 0))", slot, R(ra), R(ra + 1), R(rb)); break;
        case 12: slot = TexSlot(cbuf_off, dim, false, false);
            load = fmt::format("tex{0}.Load(int3(RsXY(int2(int({1}), int({2})), {0}u), int({3})))", slot, R(ra), R(ra + 1), R(rb)); break;
        case 14: array = true; slot = TexSlot(cbuf_off, dim, true, false);
            load = fmt::format("tex{0}.Load(int4(RsXY(int2(int({1}), int({2})), {0}u), int({3}), 0))", slot, R(ra + 1), R(rb), R(ra)); break;
        default: Unimpl(op, pc, insn); return;
        }
        if (!Bit(insn, 59)) // bit 59 clear: half-precision results packed in pairs
            EmitTexWritesF16(mask, d0, d1, load, slot);
        else
            EmitTexWrites(mask, {d0, d0 == RZ ? RZ : d0 + 1, d1, d1 == RZ ? RZ : d1 + 1}, load, slot);
        return;
    }
    case Op::TLD:
    case Op::TLD_b: {
        const u32 d = u32(Bits(insn, 0, 8));
        // 3-bit texture type: 1D, 1D array, 2D, 2D array, 3D, 3D array, cube, cube array
        const u32 type3 = u32(Bits(insn, 28, 3));
        const u32 type = type3 >> 1;
        const bool array = type3 & 1;
        const u32 mask = u32(Bits(insn, 31, 4));
        const bool lod_ll = Bit(insn, 55);
        const TexDim dim = TexDim(type);
        u32 c = ra;
        std::string aidx;
        if (array)
            aidx = fmt::format("int({})", R(c++));
        const u32 ncoord = dim == TexDim::Tex1D ? 1 : dim == TexDim::Tex2D ? 2 : 3;
        std::string coords;
        for (u32 i = 0; i < ncoord; i++)
            coords += (i ? ", " : "") + fmt::format("int({})", R(c + i));
        if (array)
            coords += ", " + aidx;
        const std::string lod = lod_ll ? fmt::format("int({})", R(rb)) : "0";
        std::vector<u32> regs;
        for (u32 i = 0; i < 4; i++)
            regs.push_back(d == RZ ? RZ : d + i);
        if (dim == TexDim::Tex1D && !array) { // texel buffer fetch
            const u32 slot = TexSlot(cbuf_off, TexDim::Buffer, false, false);
            EmitTexWrites(mask, regs, fmt::format("tex{}.Load({})", slot, coords), slot);
            return;
        }
        const u32 slot = TexSlot(cbuf_off, dim, array, false);
        const u32 n = ncoord + (array ? 1 : 0) + 1;
        if (dim == TexDim::Tex2D) { // render targets may be stored at a higher resolution (see RsXY)
            coords = fmt::format("RsXY(int2(int({}), int({})), {}u)", R(c), R(c + 1), slot);
            if (array)
                coords += ", " + aidx;
        }
        EmitTexWrites(mask, regs, fmt::format("tex{}.Load(int{}({}, {}))", slot, n, coords, lod), slot);
        return;
    }
    case Op::TLD4S: {
        const u32 d0 = u32(Bits(insn, 0, 8)), d1 = u32(Bits(insn, 28, 8));
        const bool dc = Bit(insn, 50), aoffi = Bit(insn, 51);
        const u32 comp = u32(Bits(insn, 52, 2));
        std::string coords, dref;
        if (!dc && !aoffi)
            coords = fmt::format("float2({}, {})", coord(ra), coord(rb));
        else {
            coords = fmt::format("float2({}, {})", coord(ra), coord(ra + 1));
            if (dc)
                dref = coord(rb);
        }
        const u32 slot = TexSlot(cbuf_off, TexDim::Tex2D, false, dc);
        static const char* g[] = {"GatherRed", "GatherGreen", "GatherBlue", "GatherAlpha"};
        const std::string v = dc ? fmt::format("tex{}.GatherCmp({}, {}, {})", slot, CmpSampler(slot), coords, dref)
                                 : fmt::format("tex{}.{}({}, {})", slot, g[comp], Sampler(slot), coords);
        if (Bit(insn, 55)) {
            EmitTexWritesF16(0xF, d0, d1, v, ~0u);
            return;
        }
        Line(fmt::format("tmp4 = {};", v));
        SetF(d0, "tmp4.x");
        if (d0 != RZ) SetF(d0 + 1, "tmp4.y");
        SetF(d1, "tmp4.z");
        if (d1 != RZ) SetF(d1 + 1, "tmp4.w");
        return;
    }
    case Op::TLD4:
    case Op::TLD4_b: {
        const u32 d = u32(Bits(insn, 0, 8));
        // 3-bit texture type: 1D, 1D array, 2D, 2D array, 3D, 3D array, cube, cube array
        const u32 type3 = u32(Bits(insn, 28, 3));
        const u32 type = type3 >> 1;
        const bool array = type3 & 1;
        const u32 mask = u32(Bits(insn, 31, 4));
        const bool dc = Bit(insn, 50);
        const u32 offset_type = u32(Bits(insn, 54, 2));
        const u32 comp = u32(Bits(insn, 56, 2));
        const TexDim dim = TexDim(type);
        u32 c = ra;
        std::string aidx;
        if (array)
            aidx = fmt::format("float({})", R(c++));
        const u32 ncoord = dim == TexDim::Tex2D ? 2 : 3;
        std::string coords;
        for (u32 i = 0; i < ncoord; i++)
            coords += (i ? ", " : "") + coord(c + i);
        if (array)
            coords += ", " + aidx;
        coords = fmt::format("float{}({})", ncoord + (array ? 1 : 0), coords);
        u32 meta = rb;
        if (offset_type)
            meta++;
        std::string dref = dc ? coord(meta++) : "";
        const u32 slot = TexSlot(cbuf_off, dim, array, dc);
        static const char* g[] = {"GatherRed", "GatherGreen", "GatherBlue", "GatherAlpha"};
        const std::string v = dc ? fmt::format("tex{}.GatherCmp({}, {}, {})", slot, CmpSampler(slot), coords, dref)
                                 : fmt::format("tex{}.{}({}, {})", slot, g[comp], Sampler(slot), coords);
        Line(fmt::format("tmp4 = {};", v));
        static const char* cs = "xyzw";
        u32 r = d;
        for (u32 i = 0; i < 4; i++)
            if (mask & (1u << i))
                if (d != RZ)
                    SetF(r++, fmt::format("tmp4.{}", cs[i]));
        return;
    }
    case Op::TXQ:
    case Op::TXQ_b: {
        const u32 d = u32(Bits(insn, 0, 8));
        const u32 query = u32(Bits(insn, 22, 6));
        const u32 mask = u32(Bits(insn, 31, 4));
        const u32 slot = TexSlot(cbuf_off, TexDim::Tex2D, false, false);
        if (query == 1) { // dimensions
            Line(fmt::format("tex{0}.GetDimensions({1}, tmpu.x, tmpu.y, tmpu.w); tmpu.z = 1u; tmpu.xy = tmpu.xy * 3u / max(tex_swizzle[{0}].y, 3u);",
                             slot, ra == RZ ? "0u" : R(ra)));
            static const char* cs = "xyzw";
            u32 r = d;
            for (u32 i = 0; i < 4; i++)
                if (mask & (1u << i))
                    SetR(r++, fmt::format("tmpu.{}", cs[i]));
        } else {
            Unimpl(op, pc, insn);
        }
        return;
    }
    case Op::TMML:
    case Op::TMML_b: {
        const u32 d = u32(Bits(insn, 0, 8));
        const u32 slot = TexSlot(cbuf_off, TexDim::Tex2D, false, false);
        Line(fmt::format("tmp4.x = tex{}.CalculateLevelOfDetail({}, float2({}, {}));", slot, Sampler(slot),
                         coord(ra), coord(ra + 1)));
        SetR(d, "uint(tmp4.x * 256.0f)");
        if (d != RZ)
            SetF(d + 1, "tmp4.x");
        return;
    }
    default:
        Unimpl(op, pc, insn);
    }
}

// ================================================================ flow analysis
bool Translator::DiscoverBlocks() {
    std::set<u32> leaders{8};
    std::set<u32> reachable;
    std::vector<u32> work{8};
    std::set<u32> started;
    while (!work.empty()) {
        u32 pc = work.back();
        work.pop_back();
        if (!started.insert(pc).second)
            continue;
        for (int guard = 0; guard < 100000; guard++) {
            if (!Valid(pc))
                break;
            if (reachable.count(pc))
                break;
            reachable.insert(pc);
            const u64 insn = At(pc);
            const Op op = Decode(insn);
            const u32 pred = u32(Bits(insn, 16, 3));
            const bool pneg = Bit(insn, 19);
            const bool unconditional = pred == PT && !pneg;
            const u32 next = NextPc(pc);
            bool end = false;
            switch (op) {
            case Op::BRA: {
                const u32 target = BranchTarget(pc, insn);
                const bool cc_always = Bits(insn, 0, 5) == 15;
                leaders.insert(target);
                work.push_back(target);
                if (target <= pc)
                    has_back_edge_ = true;
                if (!unconditional || !cc_always) {
                    leaders.insert(next);
                    work.push_back(next);
                }
                end = true;
                break;
            }
            case Op::SSY:
            case Op::PBK:
            case Op::PCNT: {
                const u32 target = BranchTarget(pc, insn);
                leaders.insert(target);
                work.push_back(target);
                break;
            }
            case Op::BRX:
                if (!ResolveBrx(pc, insn)) {
                    res_.error = fmt::format("BRX at {:#x}: jump table not found", pc);
                    return false;
                }
                for (auto& [value, target] : brx_[pc]) {
                    leaders.insert(target);
                    work.push_back(target);
                    if (target <= pc)
                        has_back_edge_ = true;
                }
                if (!unconditional) {
                    leaders.insert(next);
                    work.push_back(next);
                }
                end = true;
                break;
            case Op::EXIT:
            case Op::SYNC:
            case Op::BRK:
            case Op::CONT:
                if (!unconditional || Bits(insn, 0, 5) != 15) {
                    leaders.insert(next);
                    work.push_back(next);
                }
                end = true;
                break;
            case Op::Invalid:
                res_.unimplemented.insert(fmt::format("invalid_{:04x}@{:x}", u32(insn >> 48), pc));
                break;
            default:
                break;
            }
            if (end)
                break;
            pc = next;
            if (leaders.count(pc))
                break;
        }
    }
    if (reachable.empty()) {
        res_.error = "no instructions";
        return false;
    }
    // Build blocks from reachable instructions split at leaders and terminators.
    u32 id = 0;
    Block cur{};
    bool open = false;
    u32 prev = 0;
    for (u32 pc : reachable) {
        const bool new_block = !open || leaders.count(pc) || NextPc(prev) != pc;
        if (new_block) {
            if (open) {
                cur.end = NextPc(prev);
                blocks_[cur.start] = cur;
            }
            cur = Block{pc, 0, id++};
            open = true;
        }
        block_of_[pc] = cur.start;
        const Op op = Decode(At(pc));
        prev = pc;
        if (op == Op::BRA || op == Op::BRX || op == Op::EXIT || op == Op::SYNC || op == Op::BRK || op == Op::CONT) {
            cur.end = NextPc(pc);
            blocks_[cur.start] = cur;
            open = false;
        }
    }
    if (open) {
        cur.end = NextPc(prev);
        blocks_[cur.start] = cur;
    }
    // Back edges to earlier blocks also arise from SYNC/BRK/CONT targets; checked during stack analysis.
    return true;
}

bool Translator::AnalyzeStacks() {
    using Stack = std::vector<std::pair<char, u32>>;
    std::vector<std::pair<u32, Stack>> work{{8, {}}};
    while (!work.empty()) {
        auto [start, stack] = work.back();
        work.pop_back();
        if (!blocks_.count(start))
            continue;
        if (block_stack_.count(start))
            continue;
        block_stack_[start] = stack;
        const Block& b = blocks_[start];
        for (u32 pc = b.start; pc < b.end; pc = NextPc(pc)) {
            const u64 insn = At(pc);
            const Op op = Decode(insn);
            const u32 next = NextPc(pc);
            const bool unconditional = Bits(insn, 16, 4) == PT;
            auto push_successor = [&](u32 t, const Stack& s) {
                if (t <= start && blocks_.count(t))
                    has_back_edge_ = true;
                work.push_back({t, s});
            };
            switch (op) {
            case Op::SSY: stack.push_back({'S', BranchTarget(pc, insn)}); break;
            case Op::PBK: stack.push_back({'B', BranchTarget(pc, insn)}); break;
            case Op::PCNT: stack.push_back({'C', BranchTarget(pc, insn)}); break;
            case Op::SYNC:
            case Op::BRK:
            case Op::CONT: {
                const char want = op == Op::SYNC ? 'S' : op == Op::BRK ? 'B' : 'C';
                Stack s = stack;
                while (!s.empty() && s.back().first != want)
                    s.pop_back();
                if (s.empty()) {
                    res_.unimplemented.insert("unbalanced_sync_stack");
                    sync_target_[pc] = 0xFFFFFFFF;
                    break;
                }
                const u32 target = s.back().second;
                if (want != 'C')
                    s.pop_back();
                sync_target_[pc] = target;
                push_successor(target, s);
                if (!unconditional || Bits(insn, 0, 5) != 15)
                    push_successor(next, stack);
                break;
            }
            case Op::BRA: {
                const u32 target = BranchTarget(pc, insn);
                push_successor(target, stack);
                if (!unconditional || Bits(insn, 0, 5) != 15)
                    push_successor(next, stack);
                break;
            }
            case Op::BRX:
                for (auto& [value, target] : brx_[pc])
                    push_successor(target, stack);
                if (!unconditional)
                    push_successor(next, stack);
                break;
            case Op::EXIT:
                if (!unconditional || Bits(insn, 0, 5) != 15)
                    push_successor(next, stack);
                break;
            default:
                break;
            }
            if (next == b.end && op != Op::BRA && op != Op::BRX && op != Op::EXIT && op != Op::SYNC && op != Op::BRK &&
                op != Op::CONT)
                push_successor(next, stack);
        }
    }
    return true;
}

// BRX Ra, offset: target = pc + 8 + offset + Ra, where Ra was loaded from a jump table in constant buffer 1
// (LDC c[1][Ri + off]) after the index was clamped (IMNMX with an immediate bound). Reads the table entries.
bool Translator::ResolveBrx(u32 pc, u64 insn) {
    if (brx_.count(pc))
        return true;
    const u32 ra = u32(Bits(insn, 8, 8));
    const s32 off = s32(u32(Bits(insn, 20, 24)) << 8) >> 8;
    const u32 base = u32(s32(pc + 8) + off);
    u32 cb_index = ~0u, cb_off = 0;
    s32 bound = -1;
    for (u32 q = pc, steps = 0; q >= 16 && steps < 48; steps++) {
        q -= 8;
        if (q % 32 == 0)
            continue;
        const u64 j = At(q);
        const Op o = Decode(j);
        if (cb_index == ~0u) {
            if (o == Op::LDC && u32(Bits(j, 0, 8)) == ra) {
                cb_index = u32(Bits(j, 36, 5));
                cb_off = u32(s32(u32(Bits(j, 20, 16)) << 16) >> 16);
            }
            continue;
        }
        if (o == Op::IMNMX_imm) {
            bound = s32(u32(Bits(j, 20, 19)) | (Bit(j, 56) ? 0xFFF80000u : 0u));
            break;
        }
    }
    if (cb_index != 1 || bound < 0 || bound > 255 || !opt_.cbuf1)
        return false;
    auto& entries = brx_[pc];
    for (u32 k = 0; k <= u32(bound); k++) {
        const u32 at = cb_off + k * 4;
        if (at + 4 > opt_.cbuf1_size)
            return false;
        u32 v;
        std::memcpy(&v, opt_.cbuf1 + at, 4);
        u32 t = base + v;
        if (t % 32 == 0) // never a scheduling control word
            t += 8;
        if (!Valid(t))
            return false;
        entries.push_back({v, t});
    }
    return true;
}

void Translator::EmitJump(u32 target) {
    auto it = blocks_.find(target);
    if (it == blocks_.end()) {
        Line("return_flag = true; return; // jump outside program");
        return;
    }
    Line(fmt::format("blk = {}u;", it->second.id));
    if (it->second.id <= blocks_[cur_block_].id)
        Line("continue;");
}

// ================================================================ instructions
void Translator::EmitInstruction(u32 pc, u64 insn, Op op) {
    const u32 d = u32(Bits(insn, 0, 8));
    const u32 a = u32(Bits(insn, 8, 8));
    const bool cc = Bit(insn, 47);
    // operand form helpers
    auto form3 = [](Op o, Op reg, Op cbuf) { return o == reg ? 0 : o == cbuf ? 1 : 2; };

    switch (op) {
    case Op::NOP: case Op::DEPBAR: case Op::MEMBAR: case Op::RET: case Op::SSY: case Op::PBK: case Op::PCNT:
    case Op::CCTL: case Op::CCTLL: case Op::FCHK_reg: case Op::FCHK_cbuf: case Op::FCHK_imm:
        return;

    // ---------------- moves
    case Op::MOV_reg: SetR(d, R(u32(Bits(insn, 20, 8)))); return;
    case Op::MOV_cbuf: SetR(d, CbufOperand(insn)); return;
    case Op::MOV_imm: SetR(d, IntImm20(insn)); return;
    case Op::MOV32I: SetR(d, Imm32(insn)); return;
    case Op::SEL_reg: case Op::SEL_cbuf: case Op::SEL_imm: {
        const std::string b = SrcB(insn, form3(op, Op::SEL_reg, Op::SEL_cbuf));
        SetR(d, fmt::format("{} ? {} : {}", P(u32(Bits(insn, 39, 3)), Bit(insn, 42)), R(a), b));
        return;
    }

    // ---------------- float arithmetic
    case Op::FADD_reg: case Op::FADD_cbuf: case Op::FADD_imm: {
        const std::string b = SrcBF(insn, form3(op, Op::FADD_reg, Op::FADD_cbuf));
        const std::string v = Sat(fmt::format("{} + {}", FAbsNeg(F(a), Bit(insn, 46), Bit(insn, 48)),
                                              FAbsNeg(b, Bit(insn, 49), Bit(insn, 45))),
                                  Bit(insn, 50));
        SetF(d, v);
        if (cc) SetCcFloat(R(d));
        return;
    }
    case Op::FADD32I: {
        const std::string v = fmt::format("{} + {}", FAbsNeg(F(a), Bit(insn, 54), Bit(insn, 56)),
                                          FAbsNeg("asfloat(" + Imm32(insn) + ")", Bit(insn, 57), Bit(insn, 53)));
        SetF(d, v);
        if (Bit(insn, 52)) SetCcFloat(R(d));
        return;
    }
    case Op::FMUL_reg: case Op::FMUL_cbuf: case Op::FMUL_imm: {
        const std::string b = SrcBF(insn, form3(op, Op::FMUL_reg, Op::FMUL_cbuf));
        static const char* scale[] = {"", " * 0.5f", " * 0.25f", " * 0.125f", " * 8.0f", " * 4.0f", " * 2.0f", ""};
        std::string v = fmt::format("{} * {}{}", F(a), FAbsNeg(b, false, Bit(insn, 48)), scale[Bits(insn, 41, 3)]);
        SetF(d, Sat(v, Bit(insn, 50)));
        if (cc) SetCcFloat(R(d));
        return;
    }
    case Op::FMUL32I:
        SetF(d, Sat(fmt::format("{} * asfloat({})", F(a), Imm32(insn)), Bit(insn, 55)));
        if (Bit(insn, 52)) SetCcFloat(R(d));
        return;
    case Op::FFMA_reg: case Op::FFMA_rc: case Op::FFMA_cr: case Op::FFMA_imm: {
        std::string b, c;
        if (op == Op::FFMA_reg) { b = F(u32(Bits(insn, 20, 8))); c = F(u32(Bits(insn, 39, 8))); }
        else if (op == Op::FFMA_rc) { b = F(u32(Bits(insn, 39, 8))); c = "asfloat(" + CbufOperand(insn) + ")"; }
        else if (op == Op::FFMA_cr) { b = "asfloat(" + CbufOperand(insn) + ")"; c = F(u32(Bits(insn, 39, 8))); }
        else { b = FloatImm20(insn); c = F(u32(Bits(insn, 39, 8))); }
        SetF(d, Sat(fmt::format("mad({}, {}, {})", F(a), FAbsNeg(b, false, Bit(insn, 48)),
                                FAbsNeg(c, false, Bit(insn, 49))),
                    Bit(insn, 50)));
        if (cc) SetCcFloat(R(d));
        return;
    }
    case Op::FFMA32I:
        SetF(d, Sat(fmt::format("mad({}, asfloat({}), {})", FAbsNeg(F(a), false, Bit(insn, 56)), Imm32(insn),
                                FAbsNeg(F(d), false, Bit(insn, 57))),
                    Bit(insn, 55)));
        if (Bit(insn, 52)) SetCcFloat(R(d));
        return;
    case Op::FMNMX_reg: case Op::FMNMX_cbuf: case Op::FMNMX_imm: {
        const std::string b = FAbsNeg(SrcBF(insn, form3(op, Op::FMNMX_reg, Op::FMNMX_cbuf)), Bit(insn, 49), Bit(insn, 45));
        const std::string av = FAbsNeg(F(a), Bit(insn, 46), Bit(insn, 48));
        SetF(d, fmt::format("{} ? min({}, {}) : max({}, {})", P(u32(Bits(insn, 39, 3)), Bit(insn, 42)), av, b, av, b));
        return;
    }
    // ---------------- double precision (register pairs: low word in rN, high word in rN+1)
    case Op::DADD_reg: case Op::DADD_cbuf: case Op::DADD_imm:
    case Op::DMUL_reg: case Op::DMUL_cbuf: case Op::DMUL_imm:
    case Op::DFMA_reg: case Op::DFMA_rc: case Op::DFMA_cr: case Op::DFMA_imm:
    case Op::DMNMX_reg: case Op::DMNMX_cbuf: case Op::DMNMX_imm:
    case Op::DSETP_reg: case Op::DSETP_cbuf: case Op::DSETP_imm: {
        auto dreg = [&](u32 r) { return r == RZ ? std::string("0.0") : fmt::format("asdouble({}, {})", R(r), R(r + 1)); };
        auto dcbuf = [&]() {
            const u32 bank = u32(Bits(insn, 34, 5)), off = u32(Bits(insn, 20, 14)) * 4;
            return fmt::format("asdouble({}, {})", Cbuf(bank, off), Cbuf(bank, off + 4));
        };
        // 20-bit immediates hold the top bits of the double.
        auto dimm = [&]() {
            const u32 hi = (u32(Bits(insn, 20, 19)) << 12) | (u32(Bit(insn, 56)) << 31);
            return fmt::format("asdouble(0u, {})", Hex(hi));
        };
        auto dabsneg = [](std::string v, bool abs, bool neg) {
            if (abs) v = "abs(" + v + ")";
            if (neg) v = "(-" + v + ")";
            return v;
        };
        auto src_b = [&](Op reg, Op cbuf) {
            return op == reg ? dreg(u32(Bits(insn, 20, 8))) : op == cbuf ? dcbuf() : dimm();
        };
        auto store = [&](const std::string& v) {
            Line(fmt::format("{{ double dv_ = {}; uint lo_, hi_; asuint(dv_, lo_, hi_); {} = lo_; {} = hi_; }}", v,
                             d == RZ ? "tmpu.x" : R(d), d == RZ ? "tmpu.y" : R(d + 1)));
        };
        const std::string av = dreg(a);
        if (op == Op::DADD_reg || op == Op::DADD_cbuf || op == Op::DADD_imm) {
            store(fmt::format("{} + {}", dabsneg(av, Bit(insn, 46), Bit(insn, 48)),
                              dabsneg(src_b(Op::DADD_reg, Op::DADD_cbuf), Bit(insn, 49), Bit(insn, 45))));
        } else if (op == Op::DMUL_reg || op == Op::DMUL_cbuf || op == Op::DMUL_imm) {
            store(fmt::format("{} * {}", av, dabsneg(src_b(Op::DMUL_reg, Op::DMUL_cbuf), false, Bit(insn, 48))));
        } else if (op == Op::DMNMX_reg || op == Op::DMNMX_cbuf || op == Op::DMNMX_imm) {
            const std::string bv = dabsneg(src_b(Op::DMNMX_reg, Op::DMNMX_cbuf), Bit(insn, 49), Bit(insn, 45));
            const std::string a2 = dabsneg(av, Bit(insn, 46), Bit(insn, 48));
            store(fmt::format("{} ? min({}, {}) : max({}, {})", P(u32(Bits(insn, 39, 3)), Bit(insn, 42)), a2, bv, a2, bv));
        } else if (op == Op::DSETP_reg || op == Op::DSETP_cbuf || op == Op::DSETP_imm) {
            const std::string bv = dabsneg(src_b(Op::DSETP_reg, Op::DSETP_cbuf), Bit(insn, 44), Bit(insn, 6));
            const std::string a2 = dabsneg(av, Bit(insn, 7), Bit(insn, 43));
            const std::string sp = P(u32(Bits(insn, 39, 3)), Bit(insn, 42));
            const u32 bop = u32(Bits(insn, 45, 2));
            // isnan() has no double overload in SM5: compare through placeholders and use x != x instead.
            std::string cmp = FCompare(u32(Bits(insn, 48, 4)), "DA_", "DB_");
            for (const char* ph : {"DA_", "DB_"}) {
                const std::string from = fmt::format("isnan({})", ph), to = fmt::format("({0} != {0})", ph);
                for (size_t at; (at = cmp.find(from)) != std::string::npos;)
                    cmp.replace(at, from.size(), to);
            }
            for (auto [ph, v] : {std::pair<const char*, const std::string*>{"DA_", &a2}, {"DB_", &bv}})
                for (size_t at; (at = cmp.find(ph)) != std::string::npos;)
                    cmp.replace(at, 3, "(" + *v + ")");
            Line(fmt::format("tmpb = {};", cmp));
            SetP(u32(Bits(insn, 3, 3)), Bop(bop, "tmpb", sp));
            SetP(u32(Bits(insn, 0, 3)), Bop(bop, "!tmpb", sp));
        } else { // DFMA
            std::string b, c;
            if (op == Op::DFMA_reg) { b = dreg(u32(Bits(insn, 20, 8))); c = dreg(u32(Bits(insn, 39, 8))); }
            else if (op == Op::DFMA_rc) { b = dreg(u32(Bits(insn, 39, 8))); c = dcbuf(); }
            else if (op == Op::DFMA_cr) { b = dcbuf(); c = dreg(u32(Bits(insn, 39, 8))); }
            else { b = dimm(); c = dreg(u32(Bits(insn, 39, 8))); }
            store(fmt::format("fma({}, {}, {})", av, dabsneg(b, false, Bit(insn, 48)), dabsneg(c, false, Bit(insn, 49))));
        }
        return;
    }
    case Op::MUFU: {
        const std::string s = FAbsNeg(F(a), Bit(insn, 46), Bit(insn, 48));
        std::string v;
        switch (Bits(insn, 20, 4)) {
        case 0: v = "cos(" + s + ")"; break;
        case 1: v = "sin(" + s + ")"; break;
        case 2: v = "exp2(" + s + ")"; break;
        case 3: v = "log2(" + s + ")"; break;
        case 4: v = "(1.0f / " + s + ")"; break;
        case 5: v = "rsqrt(" + s + ")"; break;
        case 8: v = "sqrt(" + s + ")"; break;
        default: Unimpl(op, pc, insn); return;
        }
        SetF(d, Sat(v, Bit(insn, 50)));
        return;
    }
    case Op::RRO_reg: case Op::RRO_cbuf: case Op::RRO_imm:
        SetF(d, FAbsNeg(SrcBF(insn, form3(op, Op::RRO_reg, Op::RRO_cbuf)), Bit(insn, 49), Bit(insn, 45)));
        return;
    case Op::FSETP_reg: case Op::FSETP_cbuf: case Op::FSETP_imm: {
        const std::string b = FAbsNeg(SrcBF(insn, form3(op, Op::FSETP_reg, Op::FSETP_cbuf)), Bit(insn, 44), Bit(insn, 6));
        const std::string av = FAbsNeg(F(a), Bit(insn, 7), Bit(insn, 43));
        const std::string cmp = FCompare(u32(Bits(insn, 48, 4)), av, b);
        const std::string sp = P(u32(Bits(insn, 39, 3)), Bit(insn, 42));
        const u32 bop = u32(Bits(insn, 45, 2));
        Line(fmt::format("tmpb = {};", cmp));
        SetP(u32(Bits(insn, 3, 3)), Bop(bop, "tmpb", sp));
        SetP(u32(Bits(insn, 0, 3)), Bop(bop, "!tmpb", sp));
        return;
    }
    case Op::FSET_reg: case Op::FSET_cbuf: case Op::FSET_imm: {
        const std::string b = FAbsNeg(SrcBF(insn, form3(op, Op::FSET_reg, Op::FSET_cbuf)), Bit(insn, 44), Bit(insn, 53));
        const std::string av = FAbsNeg(F(a), Bit(insn, 54), Bit(insn, 43));
        const std::string cond = Bop(u32(Bits(insn, 45, 2)), FCompare(u32(Bits(insn, 48, 4)), av, b),
                                     P(u32(Bits(insn, 39, 3)), Bit(insn, 42)));
        SetR(d, fmt::format("({}) ? {} : 0u", cond, Bit(insn, 52) ? "asuint(1.0f)" : "0xFFFFFFFFu"));
        if (cc) SetCcInt(R(d));
        return;
    }
    case Op::FCMP_reg: case Op::FCMP_rc: case Op::FCMP_cr: case Op::FCMP_imm: {
        std::string b, c;
        if (op == Op::FCMP_reg) { b = R(u32(Bits(insn, 20, 8))); c = F(u32(Bits(insn, 39, 8))); }
        else if (op == Op::FCMP_rc) { b = R(u32(Bits(insn, 39, 8))); c = "asfloat(" + CbufOperand(insn) + ")"; }
        else if (op == Op::FCMP_cr) { b = CbufOperand(insn); c = F(u32(Bits(insn, 39, 8))); }
        else { b = IntImm20(insn); c = F(u32(Bits(insn, 39, 8))); }
        SetR(d, fmt::format("{} ? {} : {}", FCompare(u32(Bits(insn, 48, 4)), c, "0.0f"), R(a), b));
        return;
    }
    case Op::F2F_reg: case Op::F2F_cbuf: case Op::F2F_imm: {
        std::string s = FAbsNeg(SrcBF(insn, form3(op, Op::F2F_reg, Op::F2F_cbuf)), Bit(insn, 49), Bit(insn, 45));
        const u32 src_size = u32(Bits(insn, 10, 2)), dst_size = u32(Bits(insn, 8, 2));
        if (src_size == 1) // f16 source; bit 41 selects the high half
            s = FAbsNeg(fmt::format("f16tof32(({} >> {}u) & 0xFFFFu)", SrcB(insn, form3(op, Op::F2F_reg, Op::F2F_cbuf)),
                                    Bit(insn, 41) ? 16 : 0),
                        Bit(insn, 49), Bit(insn, 45));
        if (Bit(insn, 42)) { // round to integer
            static const char* rf[] = {"round", "floor", "ceil", "trunc"};
            s = fmt::format("{}({})", rf[Bits(insn, 39, 2)], s);
        }
        s = Sat(s, Bit(insn, 50));
        if (dst_size == 1)
            SetR(d, fmt::format("f32tof16({})", s));
        else
            SetF(d, s);
        if (cc) SetCcInt(R(d));
        return;
    }
    case Op::F2I_reg: case Op::F2I_cbuf: case Op::F2I_imm: {
        std::string s = FAbsNeg(SrcBF(insn, form3(op, Op::F2I_reg, Op::F2I_cbuf)), Bit(insn, 49), Bit(insn, 45));
        if (Bits(insn, 10, 2) == 1) // f16 source; bit 41 selects the high half
            s = FAbsNeg(fmt::format("f16tof32(({} >> {}u) & 0xFFFFu)", SrcB(insn, form3(op, Op::F2I_reg, Op::F2I_cbuf)),
                                    Bit(insn, 41) ? 16 : 0),
                        Bit(insn, 49), Bit(insn, 45));
        static const char* rf[] = {"round", "floor", "ceil", "trunc"};
        s = fmt::format("{}({})", rf[Bits(insn, 39, 2)], s);
        const bool is_signed = Bit(insn, 12);
        const u32 dst_size = u32(Bits(insn, 8, 2));
        std::string v;
        if (is_signed) {
            const char* lim = dst_size == 0 ? "-128.0f, 127.0f" : dst_size == 1 ? "-32768.0f, 32767.0f"
                                                                                : "-2147483648.0f, 2147483520.0f";
            v = fmt::format("uint(int(clamp({}, {})))", s, lim);
        } else {
            const char* lim = dst_size == 0 ? "0.0f, 255.0f" : dst_size == 1 ? "0.0f, 65535.0f" : "0.0f, 4294967040.0f";
            v = fmt::format("uint(clamp({}, {}))", s, lim);
        }
        SetR(d, v);
        if (cc) SetCcInt(R(d));
        return;
    }
    case Op::I2F_reg: case Op::I2F_cbuf: case Op::I2F_imm: {
        std::string s = SrcB(insn, form3(op, Op::I2F_reg, Op::I2F_cbuf));
        const u32 src_size = u32(Bits(insn, 10, 2));
        const u32 sel = u32(Bits(insn, 41, 2));
        const bool is_signed = Bit(insn, 13);
        if (src_size == 0) // byte
            s = is_signed ? fmt::format("uint(int({} << {}u) >> 24)", s, 24 - sel * 8) : fmt::format("(({} >> {}u) & 0xFFu)", s, sel * 8);
        else if (src_size == 1) // short
            s = is_signed ? fmt::format("uint(int({} << {}u) >> 16)", s, 16 - sel * 8) : fmt::format("(({} >> {}u) & 0xFFFFu)", s, sel * 8);
        std::string v = is_signed ? fmt::format("float(int({}))", s) : fmt::format("float({})", s);
        v = FAbsNeg(v, Bit(insn, 49), Bit(insn, 45));
        if (Bits(insn, 8, 2) == 1)
            SetR(d, fmt::format("f32tof16({})", v));
        else
            SetF(d, v);
        if (cc) SetCcInt(R(d));
        return;
    }
    case Op::I2I_reg: case Op::I2I_cbuf: case Op::I2I_imm: {
        std::string s = SrcB(insn, form3(op, Op::I2I_reg, Op::I2I_cbuf));
        const u32 src_size = u32(Bits(insn, 10, 2)), dst_size = u32(Bits(insn, 8, 2));
        const u32 sel = u32(Bits(insn, 41, 2));
        const bool src_signed = Bit(insn, 13), dst_signed = Bit(insn, 12);
        if (src_size == 0)
            s = src_signed ? fmt::format("uint(int({} << {}u) >> 24)", s, 24 - sel * 8) : fmt::format("(({} >> {}u) & 0xFFu)", s, sel * 8);
        else if (src_size == 1)
            s = src_signed ? fmt::format("uint(int({} << {}u) >> 16)", s, 16 - sel * 8) : fmt::format("(({} >> {}u) & 0xFFFFu)", s, sel * 8);
        if (Bit(insn, 49)) s = fmt::format("uint(abs(int({})))", s);
        if (Bit(insn, 45)) s = fmt::format("uint(-int({}))", s);
        if (Bit(insn, 50)) { // saturate to destination range
            if (dst_size == 0) s = dst_signed ? fmt::format("uint(clamp(int({}), -128, 127))", s) : fmt::format("min({}, 255u)", s);
            else if (dst_size == 1) s = dst_signed ? fmt::format("uint(clamp(int({}), -32768, 32767))", s) : fmt::format("min({}, 65535u)", s);
        }
        SetR(d, s);
        if (cc) SetCcInt(R(d));
        return;
    }

    // ---------------- integer arithmetic
    case Op::IADD_reg: case Op::IADD_cbuf: case Op::IADD_imm: {
        std::string b = SrcB(insn, form3(op, Op::IADD_reg, Op::IADD_cbuf));
        std::string av = R(a);
        if (Bit(insn, 49)) av = "(0u - " + av + ")";
        if (Bit(insn, 48)) b = "(0u - " + b + ")";
        std::string v = fmt::format("{} + {}", av, b);
        if (Bit(insn, 43)) { cc_used_ = true; v += " + (cc_c ? 1u : 0u)"; }
        if (cc) {
            cc_used_ = true;
            Line(fmt::format("tmpu.x = {}; tmpu.y = {}; tmpu.z = tmpu.x + tmpu.y{};", av, b,
                             Bit(insn, 43) ? " + (cc_c ? 1u : 0u)" : ""));
            Line("cc_c = tmpu.z < tmpu.x || (tmpu.z == tmpu.x && tmpu.y != 0u); cc_z = tmpu.z == 0u; cc_s = int(tmpu.z) < 0;"
                 " cc_o = ((~(tmpu.x ^ tmpu.y)) & (tmpu.x ^ tmpu.z) & 0x80000000u) != 0u;");
            SetR(d, "tmpu.z");
        } else {
            SetR(d, v);
        }
        return;
    }
    case Op::IADD32I: {
        std::string av = R(a);
        if (Bit(insn, 56)) av = "(0u - " + av + ")";
        std::string v = fmt::format("{} + {}", av, Imm32(insn));
        if (Bit(insn, 53)) { cc_used_ = true; v += " + (cc_c ? 1u : 0u)"; }
        SetR(d, v);
        if (Bit(insn, 52)) SetCcInt(R(d));
        return;
    }
    case Op::IADD3_reg: case Op::IADD3_cbuf: case Op::IADD3_imm: {
        std::string b = SrcB(insn, form3(op, Op::IADD3_reg, Op::IADD3_cbuf));
        std::string c = R(u32(Bits(insn, 39, 8)));
        std::string av = R(a);
        if (Bit(insn, 51)) av = "(0u - " + av + ")";
        if (Bit(insn, 50)) b = "(0u - " + b + ")";
        if (Bit(insn, 49)) c = "(0u - " + c + ")";
        SetR(d, fmt::format("{} + {} + {}", av, b, c));
        if (cc) SetCcInt(R(d));
        return;
    }
    case Op::ISCADD_reg: case Op::ISCADD_cbuf: case Op::ISCADD_imm: {
        std::string b = SrcB(insn, form3(op, Op::ISCADD_reg, Op::ISCADD_cbuf));
        std::string av = R(a);
        if (Bit(insn, 49)) av = "(0u - " + av + ")";
        if (Bit(insn, 48)) b = "(0u - " + b + ")";
        if (cc) {
            cc_used_ = true;
            Line(fmt::format("tmpu.x = {} << {}u; tmpu.y = {}; tmpu.z = tmpu.x + tmpu.y;", av, Bits(insn, 39, 5), b));
            Line("cc_c = tmpu.z < tmpu.x; cc_z = tmpu.z == 0u; cc_s = int(tmpu.z) < 0;"
                 " cc_o = ((~(tmpu.x ^ tmpu.y)) & (tmpu.x ^ tmpu.z) & 0x80000000u) != 0u;");
            SetR(d, "tmpu.z");
            return;
        }
        SetR(d, fmt::format("({} << {}u) + {}", av, Bits(insn, 39, 5), b));
        return;
    }
    case Op::ISCADD32I:
        SetR(d, fmt::format("({} << {}u) + {}", R(a), Bits(insn, 53, 5), Imm32(insn)));
        return;
    case Op::IMUL_reg: case Op::IMUL_cbuf: case Op::IMUL_imm: {
        const std::string b = SrcB(insn, form3(op, Op::IMUL_reg, Op::IMUL_cbuf));
        const bool high = Bit(insn, 39);
        if (high)
            Line(fmt::format("{{ uint hi_, lo_; {}(hi_, lo_, {}, {}); {} = hi_; }}",
                             Bit(insn, 40) ? "imul" : "umul", R(a), b, d == RZ ? "tmpu.x" : R(d)));
        else
            SetR(d, fmt::format("{} * {}", R(a), b));
        return;
    }
    case Op::IMUL32I:
        SetR(d, fmt::format("{} * {}", R(a), Imm32(insn)));
        return;
    case Op::IMAD_reg: case Op::IMAD_rc: case Op::IMAD_cr: case Op::IMAD_imm: {
        std::string b, c;
        if (op == Op::IMAD_reg) { b = R(u32(Bits(insn, 20, 8))); c = R(u32(Bits(insn, 39, 8))); }
        else if (op == Op::IMAD_rc) { b = R(u32(Bits(insn, 39, 8))); c = CbufOperand(insn); }
        else if (op == Op::IMAD_cr) { b = CbufOperand(insn); c = R(u32(Bits(insn, 39, 8))); }
        else { b = IntImm20(insn); c = R(u32(Bits(insn, 39, 8))); }
        SetR(d, fmt::format("{} * {} + {}", R(a), b, c));
        return;
    }
    case Op::IMNMX_reg: case Op::IMNMX_cbuf: case Op::IMNMX_imm: {
        const std::string b = SrcB(insn, form3(op, Op::IMNMX_reg, Op::IMNMX_cbuf));
        const bool s = Bit(insn, 48);
        const std::string p = P(u32(Bits(insn, 39, 3)), Bit(insn, 42));
        if (s)
            SetR(d, fmt::format("uint({} ? min(int({}), int({})) : max(int({}), int({})))", p, R(a), b, R(a), b));
        else
            SetR(d, fmt::format("{} ? min({}, {}) : max({}, {})", p, R(a), b, R(a), b));
        if (cc) SetCcInt(R(d));
        return;
    }
    case Op::ISETP_reg: case Op::ISETP_cbuf: case Op::ISETP_imm: {
        const std::string b = SrcB(insn, form3(op, Op::ISETP_reg, Op::ISETP_cbuf));
        const std::string cmp = ICompare(u32(Bits(insn, 49, 3)), Bit(insn, 48), R(a), b);
        const std::string sp = P(u32(Bits(insn, 39, 3)), Bit(insn, 42));
        const u32 bop = u32(Bits(insn, 45, 2));
        Line(fmt::format("tmpb = {};", cmp));
        SetP(u32(Bits(insn, 3, 3)), Bop(bop, "tmpb", sp));
        SetP(u32(Bits(insn, 0, 3)), Bop(bop, "!tmpb", sp));
        return;
    }
    case Op::ISET_reg: case Op::ISET_cbuf: case Op::ISET_imm: {
        const std::string b = SrcB(insn, form3(op, Op::ISET_reg, Op::ISET_cbuf));
        const std::string cond = Bop(u32(Bits(insn, 45, 2)), ICompare(u32(Bits(insn, 49, 3)), Bit(insn, 48), R(a), b),
                                     P(u32(Bits(insn, 39, 3)), Bit(insn, 42)));
        SetR(d, fmt::format("({}) ? {} : 0u", cond, Bit(insn, 44) ? "asuint(1.0f)" : "0xFFFFFFFFu"));
        if (cc) SetCcInt(R(d));
        return;
    }
    case Op::ICMP_reg: case Op::ICMP_rc: case Op::ICMP_cr: case Op::ICMP_imm: {
        std::string b, c;
        if (op == Op::ICMP_reg) { b = R(u32(Bits(insn, 20, 8))); c = R(u32(Bits(insn, 39, 8))); }
        else if (op == Op::ICMP_rc) { b = R(u32(Bits(insn, 39, 8))); c = CbufOperand(insn); }
        else if (op == Op::ICMP_cr) { b = CbufOperand(insn); c = R(u32(Bits(insn, 39, 8))); }
        else { b = IntImm20(insn); c = R(u32(Bits(insn, 39, 8))); }
        SetR(d, fmt::format("{} ? {} : {}", ICompare(u32(Bits(insn, 49, 3)), Bit(insn, 48), c, "0u"), R(a), b));
        return;
    }
    case Op::XMAD_reg: case Op::XMAD_rc: case Op::XMAD_cr: case Op::XMAD_imm: {
        std::string b, c;
        bool half_b = false, psl = false, mrg = false;
        u32 mode = 0;
        if (op == Op::XMAD_reg) {
            b = R(u32(Bits(insn, 20, 8))); c = R(u32(Bits(insn, 39, 8)));
            half_b = Bit(insn, 35); psl = Bit(insn, 36); mrg = Bit(insn, 37); mode = u32(Bits(insn, 50, 3));
        } else if (op == Op::XMAD_rc) {
            b = R(u32(Bits(insn, 39, 8))); c = CbufOperand(insn);
            half_b = Bit(insn, 52); mode = u32(Bits(insn, 50, 2));
        } else if (op == Op::XMAD_cr) {
            b = CbufOperand(insn); c = R(u32(Bits(insn, 39, 8)));
            half_b = Bit(insn, 52); psl = Bit(insn, 55); mrg = Bit(insn, 56); mode = u32(Bits(insn, 50, 2));
        } else {
            b = Hex(u32(Bits(insn, 20, 16))); c = R(u32(Bits(insn, 39, 8)));
            psl = Bit(insn, 36); mrg = Bit(insn, 37); mode = u32(Bits(insn, 50, 3));
        }
        const bool sa = Bit(insn, 48), sb = Bit(insn, 49), half_a = Bit(insn, 53);
        auto half = [](const std::string& v, bool hi, bool s) {
            if (s)
                return hi ? fmt::format("uint(int({}) >> 16)", v) : fmt::format("uint(int({} << 16u) >> 16)", v);
            return hi ? fmt::format("({} >> 16u)", v) : fmt::format("({} & 0xFFFFu)", v);
        };
        Line(fmt::format("tmpu.x = {} * {};", half(R(a), half_a, sa), half(b, half_b, sb)));
        if (psl)
            Line("tmpu.x <<= 16u;");
        std::string cv;
        switch (mode) {
        case 1: cv = fmt::format("({} & 0xFFFFu)", c); break;
        case 2: cv = fmt::format("({} >> 16u)", c); break;
        case 4: cv = fmt::format("({} + ({} << 16u))", c, b); break;
        default: cv = c; break;
        }
        Line(fmt::format("tmpu.x += {};", cv));
        if (mrg)
            Line(fmt::format("tmpu.x = (tmpu.x & 0xFFFFu) | ({} << 16u);", b));
        SetR(d, "tmpu.x");
        if (cc) SetCcInt(R(d));
        return;
    }

    // ---------------- logic / bits
    case Op::LOP_reg: case Op::LOP_cbuf: case Op::LOP_imm: {
        std::string b = SrcB(insn, form3(op, Op::LOP_reg, Op::LOP_cbuf));
        std::string av = R(a);
        if (Bit(insn, 39)) av = "(~" + av + ")";
        if (Bit(insn, 40)) b = "(~" + b + ")";
        static const char* ops[] = {"&", "|", "^"};
        const u32 lo = u32(Bits(insn, 41, 2));
        const std::string v = lo == 3 ? b : fmt::format("{} {} {}", av, ops[lo], b);
        Line(fmt::format("tmpu.x = {};", v));
        const u32 pred_op = u32(Bits(insn, 44, 2));
        const u32 dp = u32(Bits(insn, 48, 3));
        if (pred_op == 2) SetP(dp, "tmpu.x == 0u");
        else if (pred_op == 3) SetP(dp, "tmpu.x != 0u");
        else if (pred_op == 0) SetP(dp, "false");
        else SetP(dp, "true");
        SetR(d, "tmpu.x");
        if (cc) SetCcInt("tmpu.x");
        return;
    }
    case Op::LOP32I: {
        std::string av = R(a), b = Imm32(insn);
        if (Bit(insn, 55)) av = "(~" + av + ")";
        if (Bit(insn, 56)) b = "(~" + b + ")";
        static const char* ops[] = {"&", "|", "^"};
        const u32 lo = u32(Bits(insn, 53, 2));
        SetR(d, lo == 3 ? b : fmt::format("{} {} {}", av, ops[lo], b));
        if (Bit(insn, 52)) SetCcInt(R(d));
        return;
    }
    case Op::LOP3_reg: case Op::LOP3_cbuf: case Op::LOP3_imm: {
        std::string b;
        u32 lut;
        if (op == Op::LOP3_reg) { b = R(u32(Bits(insn, 20, 8))); lut = u32(Bits(insn, 28, 8)); }
        else if (op == Op::LOP3_cbuf) { b = CbufOperand(insn); lut = u32(Bits(insn, 48, 8)); }
        else { b = IntImm20(insn); lut = u32(Bits(insn, 48, 8)); }
        const std::string c = R(u32(Bits(insn, 39, 8)));
        Line(fmt::format("tmpu.x = {}; tmpu.y = {}; tmpu.z = {}; tmpu.w = 0u;", R(a), b, c));
        for (u32 i = 0; i < 8; i++) {
            if (!(lut & (1u << i)))
                continue;
            Line(fmt::format("tmpu.w |= {} & {} & {};", (i & 1) ? "tmpu.x" : "~tmpu.x", (i & 2) ? "tmpu.y" : "~tmpu.y",
                             (i & 4) ? "tmpu.z" : "~tmpu.z"));
        }
        SetR(d, "tmpu.w");
        if (op == Op::LOP3_reg) {
            const u32 dp = u32(Bits(insn, 48, 3));
            const u32 pop = u32(Bits(insn, 36, 2));
            if (pop == 2) SetP(dp, "tmpu.w == 0u");
            else if (pop == 3) SetP(dp, "tmpu.w != 0u");
        }
        if (cc) SetCcInt("tmpu.w");
        return;
    }
    case Op::SHL_reg: case Op::SHL_cbuf: case Op::SHL_imm: {
        const std::string b = SrcB(insn, form3(op, Op::SHL_reg, Op::SHL_cbuf));
        if (Bit(insn, 39))
            SetR(d, fmt::format("{} << ({} & 31u)", R(a), b));
        else
            SetR(d, fmt::format("({} >= 32u) ? 0u : ({} << {})", b, R(a), b));
        if (cc) SetCcInt(R(d));
        return;
    }
    case Op::SHR_reg: case Op::SHR_cbuf: case Op::SHR_imm: {
        const std::string b = SrcB(insn, form3(op, Op::SHR_reg, Op::SHR_cbuf));
        std::string av = R(a);
        if (Bit(insn, 40)) av = "reversebits(" + av + ")";
        const bool s = Bit(insn, 48);
        const std::string sh = Bit(insn, 39) ? fmt::format("({} & 31u)", b) : fmt::format("min({}, 31u)", b);
        if (s)
            SetR(d, fmt::format("uint(int({}) >> {})", av, sh));
        else
            SetR(d, Bit(insn, 39) ? fmt::format("{} >> {}", av, sh) : fmt::format("({} >= 32u) ? 0u : ({} >> {})", b, av, b));
        if (cc) SetCcInt(R(d));
        return;
    }
    case Op::SHF_l_reg: case Op::SHF_l_imm: case Op::SHF_r_reg: case Op::SHF_r_imm: {
        const bool left = op == Op::SHF_l_reg || op == Op::SHF_l_imm;
        const std::string b = (op == Op::SHF_l_reg || op == Op::SHF_r_reg) ? R(u32(Bits(insn, 20, 8))) : Hex(u32(Bits(insn, 20, 6)));
        const std::string hi = R(u32(Bits(insn, 39, 8)));
        Line(fmt::format("tmpu.x = {} & 63u;", b));
        if (left)
            SetR(d, fmt::format("tmpu.x == 0u ? {} : (tmpu.x < 32u ? (({} << tmpu.x) | ({} >> (32u - tmpu.x))) : ({} << (tmpu.x - 32u)))",
                                hi, hi, R(a), R(a)));
        else
            SetR(d, fmt::format("tmpu.x == 0u ? {} : (tmpu.x < 32u ? (({} >> tmpu.x) | ({} << (32u - tmpu.x))) : ({} >> (tmpu.x - 32u)))",
                                R(a), R(a), hi, hi));
        return;
    }
    case Op::BFE_reg: case Op::BFE_cbuf: case Op::BFE_imm: {
        const std::string b = SrcB(insn, form3(op, Op::BFE_reg, Op::BFE_cbuf));
        std::string av = R(a);
        if (Bit(insn, 40)) av = "reversebits(" + av + ")";
        Line(fmt::format("tmpu.x = {} & 0xFFu; tmpu.y = ({} >> 8u) & 0xFFu;", b, b));
        if (Bit(insn, 48))
            SetR(d, fmt::format("tmpu.y == 0u ? 0u : uint(int({} << (32u - min(tmpu.x + tmpu.y, 32u))) >> (32u - tmpu.y))", av));
        else
            SetR(d, fmt::format("tmpu.y == 0u ? 0u : (({} >> tmpu.x) & (tmpu.y >= 32u ? 0xFFFFFFFFu : ((1u << tmpu.y) - 1u)))", av));
        if (cc) SetCcInt(R(d));
        return;
    }
    case Op::BFI_reg: case Op::BFI_rc: case Op::BFI_cr: case Op::BFI_imm: {
        std::string b, c;
        if (op == Op::BFI_reg) { b = R(u32(Bits(insn, 20, 8))); c = R(u32(Bits(insn, 39, 8))); }
        else if (op == Op::BFI_rc) { b = R(u32(Bits(insn, 39, 8))); c = CbufOperand(insn); }
        else if (op == Op::BFI_cr) { b = CbufOperand(insn); c = R(u32(Bits(insn, 39, 8))); }
        else { b = IntImm20(insn); c = R(u32(Bits(insn, 39, 8))); }
        Line(fmt::format("tmpu.x = {} & 0xFFu; tmpu.y = ({} >> 8u) & 0xFFu;", b, b));
        Line("tmpu.z = tmpu.y >= 32u ? 0xFFFFFFFFu : ((1u << tmpu.y) - 1u);");
        SetR(d, fmt::format("({} & ~(tmpu.z << tmpu.x)) | (({} & tmpu.z) << tmpu.x)", c, R(a)));
        return;
    }
    case Op::POPC_reg: case Op::POPC_cbuf: case Op::POPC_imm: {
        std::string b = SrcB(insn, form3(op, Op::POPC_reg, Op::POPC_cbuf));
        if (Bit(insn, 40)) b = "~" + b;
        SetR(d, "countbits(" + b + ")");
        return;
    }
    case Op::FLO_reg: case Op::FLO_cbuf: case Op::FLO_imm: {
        std::string b = SrcB(insn, form3(op, Op::FLO_reg, Op::FLO_cbuf));
        if (Bit(insn, 40)) b = "~" + b;
        std::string v = Bit(insn, 48) ? fmt::format("uint(firstbithigh(int({})))", b) : fmt::format("firstbithigh({})", b);
        if (Bit(insn, 41)) v = fmt::format("({} == 0xFFFFFFFFu ? 0xFFFFFFFFu : 31u - {})", v, v);
        SetR(d, v);
        return;
    }
    case Op::PRMT_reg: case Op::PRMT_rc: case Op::PRMT_cr: case Op::PRMT_imm: {
        std::string b, c;
        if (op == Op::PRMT_reg) { b = R(u32(Bits(insn, 20, 8))); c = R(u32(Bits(insn, 39, 8))); }
        else if (op == Op::PRMT_rc) { b = R(u32(Bits(insn, 39, 8))); c = CbufOperand(insn); }
        else if (op == Op::PRMT_cr) { b = CbufOperand(insn); c = R(u32(Bits(insn, 39, 8))); }
        else { b = IntImm20(insn); c = R(u32(Bits(insn, 39, 8))); }
        if (Bits(insn, 48, 3) != 0)
            res_.unimplemented.insert("PRMT_mode");
        Line(fmt::format("tmpu.x = {}; tmpu.y = {}; tmpu.z = {}; tmpu.w = 0u;", R(a), c, b));
        Line("[unroll] for (uint pi = 0u; pi < 4u; pi++) { uint s_ = (tmpu.z >> (pi * 4u)) & 7u; uint sg_ = (tmpu.z >> (pi * 4u + 3u)) & 1u;"
             " uint by_ = s_ < 4u ? ((tmpu.x >> (s_ * 8u)) & 0xFFu) : ((tmpu.y >> ((s_ - 4u) * 8u)) & 0xFFu);"
             " if (sg_ != 0u) by_ = (by_ & 0x80u) != 0u ? 0xFFu : 0u; tmpu.w |= by_ << (pi * 8u); }");
        SetR(d, "tmpu.w");
        return;
    }
    case Op::LEA_lo_reg: case Op::LEA_lo_cbuf: case Op::LEA_lo_imm: {
        const std::string b = op == Op::LEA_lo_reg ? R(u32(Bits(insn, 20, 8))) : op == Op::LEA_lo_cbuf ? CbufOperand(insn) : IntImm20(insn);
        const u32 shift = op == Op::LEA_lo_cbuf ? u32(Bits(insn, 47, 5)) : u32(Bits(insn, 39, 5));
        std::string av = R(a);
        if (Bit(insn, 45)) av = "(0u - " + av + ")";
        const std::string xin = Bit(insn, 46) ? " + (cc_c ? 1u : 0u)" : "";
        if (Bit(insn, 47)) { // .CC: the carry feeds the LEA.HI.X computing the address's upper word
            cc_used_ = true;
            Line(fmt::format("tmpu.x = {} << {}u; tmpu.y = {}; tmpu.z = tmpu.x + tmpu.y{};", av, shift, b, xin));
            Line("cc_c = tmpu.z < tmpu.x || (tmpu.z == tmpu.x && tmpu.y != 0u); cc_z = tmpu.z == 0u; cc_s = int(tmpu.z) < 0;");
            SetR(d, "tmpu.z");
            return;
        }
        if (!xin.empty())
            cc_used_ = true;
        SetR(d, fmt::format("({} << {}u) + {}{}", av, shift, b, xin));
        return;
    }
    case Op::LEA_hi_reg: case Op::LEA_hi_cbuf: {
        // d = low word of ((c:a) >> (32 - scale)) + b: the upper half of a scaled 64-bit offset.
        const bool reg = op == Op::LEA_hi_reg;
        const std::string b = reg ? R(u32(Bits(insn, 20, 8))) : CbufOperand(insn);
        const u32 scale = reg ? u32(Bits(insn, 28, 5)) : u32(Bits(insn, 51, 5));
        const bool neg = reg ? Bit(insn, 37) : Bit(insn, 56);
        const bool x = reg ? Bit(insn, 38) : Bit(insn, 57);
        const u32 hs = 32 - scale;
        Line(fmt::format("tmpu.x = {}; tmpu.y = {};", R(a), R(u32(Bits(insn, 39, 8)))));
        if (neg) // 64-bit negate
            Line("tmpu.y = ~tmpu.y + (tmpu.x == 0u ? 1u : 0u); tmpu.x = 0u - tmpu.x;");
        if (hs == 0)
            Line("tmpu.z = tmpu.x;");
        else if (hs >= 32)
            Line(fmt::format("tmpu.z = tmpu.y >> {}u;", hs - 32));
        else
            Line(fmt::format("tmpu.z = (tmpu.x >> {}u) | (tmpu.y << {}u);", hs, 32 - hs));
        if (x)
            cc_used_ = true;
        SetR(d, fmt::format("tmpu.z + {}{}", b, x ? " + (cc_c ? 1u : 0u)" : ""));
        return;
    }

    // ---------------- predicates
    case Op::PSETP: {
        const std::string pa = P(u32(Bits(insn, 12, 3)), Bit(insn, 15));
        const std::string pb = P(u32(Bits(insn, 29, 3)), Bit(insn, 32));
        const std::string pc_ = P(u32(Bits(insn, 39, 3)), Bit(insn, 42));
        Line(fmt::format("tmpb = {};", Bop(u32(Bits(insn, 24, 2)), pa, pb)));
        const u32 bop2 = u32(Bits(insn, 45, 2));
        SetP(u32(Bits(insn, 3, 3)), Bop(bop2, "tmpb", pc_));
        SetP(u32(Bits(insn, 0, 3)), Bop(bop2, "!tmpb", pc_));
        return;
    }
    case Op::PSET: {
        const std::string pa = P(u32(Bits(insn, 12, 3)), Bit(insn, 15));
        const std::string pb = P(u32(Bits(insn, 29, 3)), Bit(insn, 32));
        const std::string pc_ = P(u32(Bits(insn, 39, 3)), Bit(insn, 42));
        const std::string cond = Bop(u32(Bits(insn, 45, 2)), Bop(u32(Bits(insn, 24, 2)), pa, pb), pc_);
        SetR(d, fmt::format("({}) ? {} : 0u", cond, Bit(insn, 44) ? "asuint(1.0f)" : "0xFFFFFFFFu"));
        return;
    }
    case Op::P2R_reg: case Op::P2R_cbuf: case Op::P2R_imm: {
        const std::string mask = op == Op::P2R_reg ? R(u32(Bits(insn, 20, 8))) : op == Op::P2R_cbuf ? CbufOperand(insn) : IntImm20(insn);
        const u32 shift = u32(Bits(insn, 41, 2)) * 8;
        std::string bits = "0u";
        for (u32 i = 0; i < 7; i++)
            bits += fmt::format(" | ({} ? {}u : 0u)", P(i, false), 1u << i);
        Line(fmt::format("tmpu.x = ({}) << {}u; tmpu.y = {} << {}u;", bits, shift, mask, shift));
        SetR(d, fmt::format("({} & ~tmpu.y) | (tmpu.x & tmpu.y)", R(a)));
        return;
    }
    case Op::R2P_reg: case Op::R2P_cbuf: case Op::R2P_imm: {
        const std::string mask = op == Op::R2P_reg ? R(u32(Bits(insn, 20, 8))) : op == Op::R2P_cbuf ? CbufOperand(insn) : IntImm20(insn);
        const u32 shift = u32(Bits(insn, 41, 2)) * 8;
        Line(fmt::format("tmpu.x = {} >> {}u; tmpu.y = {};", R(a), shift, mask));
        for (u32 i = 0; i < 7; i++)
            Line(fmt::format("if ((tmpu.y >> {}u) & 1u) p{} = ((tmpu.x >> {}u) & 1u) != 0u;", i, i, i)), preds_used_.insert(i);
        return;
    }
    case Op::CSETP: {
        const std::string t = CcTest(u32(Bits(insn, 8, 5)));
        const std::string sp = P(u32(Bits(insn, 39, 3)), Bit(insn, 42));
        const u32 bop = u32(Bits(insn, 45, 2));
        SetP(u32(Bits(insn, 3, 3)), Bop(bop, t, sp));
        SetP(u32(Bits(insn, 0, 3)), Bop(bop, "!" + t, sp));
        return;
    }

    // ---------------- system values / attributes / memory
    case Op::S2R: {
        const u32 sr = u32(Bits(insn, 20, 8));
        std::string v;
        switch (sr) {
        case 0x00: v = "0u"; break; // lane id
        case 0x12: v = "asuint(1.0f)"; break; // y direction
        case 0x21: v = "local_id.x"; break;
        case 0x22: v = "local_id.y"; break;
        case 0x23: v = "local_id.z"; break;
        case 0x25: v = "group_id.x"; break;
        case 0x26: v = "group_id.y"; break;
        case 0x27: v = "group_id.z"; break;
        case 0x50: case 0x51: v = "0u"; break;
        // Lane masks for a one-lane warp (lane 0): EQ/LE/GE contain it, LT/GT are empty.
        case 0x38: case 0x3A: case 0x3C: v = "1u"; break;
        case 0x39: case 0x3B: v = "0u"; break;
        default:
            res_.unimplemented.insert(fmt::format("S2R_{:#x}", sr));
            v = "0u";
        }
        if (stage_ != Stage::Compute && (sr >= 0x21 && sr <= 0x27))
            v = "0u";
        SetR(d, v);
        return;
    }
    case Op::ALD: {
        const u32 off = u32(Bits(insn, 20, 10));
        const u32 n = u32(Bits(insn, 47, 2)) + 1;
        if (a != RZ)
            res_.unimplemented.insert("ALD_indexed");
        for (u32 i = 0; i < n; i++)
            SetR(d == RZ ? RZ : d + i, AttrRead(off + i * 4));
        return;
    }
    case Op::AST: {
        const u32 off = u32(Bits(insn, 20, 10));
        const u32 n = u32(Bits(insn, 47, 2)) + 1;
        if (a != RZ)
            res_.unimplemented.insert("AST_indexed");
        for (u32 i = 0; i < n; i++)
            AttrWrite(off + i * 4, R(d == RZ ? RZ : d + i));
        return;
    }
    case Op::IPA: {
        const u32 attr = u32(Bits(insn, 28, 10));
        std::string v = AttrRead(attr);
        if (Bit(insn, 51))
            v = fmt::format("asuint(saturate(asfloat({})))", v);
        SetR(d, v);
        return;
    }
    case Op::LDC: {
        const u32 index = u32(Bits(insn, 36, 5));
        const s32 off = s32(u32(Bits(insn, 20, 16)) << 16) >> 16;
        const u32 size = u32(Bits(insn, 48, 3));
        if (a == RZ) {
            SetR(d, Cbuf(index, u32(off)));
            if (size == 5 && d != RZ)
                SetR(d + 1, Cbuf(index, u32(off) + 4));
            return;
        }
        // The destination pair may overlap the address register: evaluate the address first.
        Line(fmt::format("tmpu.w = {} + {};", R(a), Hex(u32(off))));
        if (size == 5 && d != RZ) {
            Line(fmt::format("tmpu.z = {};", CbufDyn(index, "tmpu.w + 4u")));
            SetR(d, CbufDyn(index, "tmpu.w"));
            SetR(d + 1, "tmpu.z");
        } else {
            SetR(d, CbufDyn(index, "tmpu.w"));
        }
        return;
    }
    case Op::LDL: case Op::STL: {
        uses_lmem_ = true;
        const s32 off = s32(u32(Bits(insn, 20, 24)) << 8) >> 8;
        const u32 size = u32(Bits(insn, 48, 3));
        const u32 words = size == 5 ? 2 : size == 6 ? 4 : 1;
        // Evaluate the address once: a multi-word load may overwrite the address register.
        Line(fmt::format("tmpu.w = ({} + {}) >> 2u;", R(a), Hex(u32(off))));
        if (op == Op::LDL) {
            for (u32 i = 0; i < words; i++)
                Line(fmt::format("tmpu4_[{}] = lmem[(tmpu.w + {}u) & 1023u];", i, i));
            for (u32 i = 0; i < words; i++)
                SetR(d == RZ ? RZ : d + i, fmt::format("tmpu4_[{}]", i));
        } else {
            for (u32 i = 0; i < words; i++)
                Line(fmt::format("lmem[(tmpu.w + {}u) & 1023u] = {};", i, R(d == RZ ? RZ : d + i)));
        }
        return;
    }
    case Op::SUST: {
        // Formatted (.P) store to a bound image: the texel is packed and written to guest memory through a global
        // memory window, at its block-linear address (drv_image[k] describes image k).
        const bool bound = Bit(insn, 51), raw = Bit(insn, 52);
        if (stage_ != Stage::Compute || !bound || raw) {
            res_.unimplemented.insert(raw ? "SUST_D" : bound ? "SUST" : "SUST_bindless");
            return;
        }
        uses_gmem_ = true;
        const u32 handle_off = u32(Bits(insn, 36, 13)) * 4;
        u32 k = 0;
        while (k < res_.images.size() && res_.images[k] != handle_off)
            k++;
        if (k == res_.images.size())
            res_.images.push_back(handle_off);
        const u32 type = u32(Bits(insn, 33, 3)); // 0 1D, 1 1D buffer, 2 1D array, 3 2D, 4 2D array, 5 3D
        const u32 cr = a;
        std::string x = R(cr), y = "0u", z = "0u";
        if (type == 3 || type == 4 || type == 5)
            y = R(cr == RZ ? RZ : cr + 1);
        if (type == 4 || type == 5)
            z = R(cr == RZ ? RZ : cr + 2);
        if (type == 2)
            z = R(cr == RZ ? RZ : cr + 1);
        const u32 mask = u32(Bits(insn, 20, 4));
        std::string v[4] = {"0u", "0u", "0u", "0u"};
        u32 reg = d;
        for (u32 c = 0; c < 4; c++)
            if (mask & (1u << c))
                v[c] = R(reg == RZ ? RZ : reg++);
        gmem_written_ = true;
        Line(fmt::format("ImageStore({}u, {}, {}, {}, uint4({}, {}, {}, {}), {}u);", k, x, y, z, v[0], v[1], v[2], v[3], mask));
        return;
    }
    case Op::CAL: {
        // Subroutines here are short straight-line helpers ending in RET: inline the body at the call site.
        u32 t = BranchTarget(pc, insn);
        for (u32 n = 0; n < 256 && Valid(t); n++, t = NextPc(t)) {
            const u64 si = At(t);
            const Op so = Decode(si);
            if (so == Op::RET)
                return;
            if (so == Op::BRA || so == Op::BRX || so == Op::CAL || so == Op::SSY || so == Op::SYNC || so == Op::EXIT ||
                so == Op::PBK || so == Op::BRK) {
                res_.unimplemented.insert("CAL_branching");
                return;
            }
            const u32 pidx = u32(Bits(si, 16, 3));
            const bool pneg = Bit(si, 19);
            const bool guarded = !(pidx == PT && !pneg);
            if (guarded) {
                Line(fmt::format("if ({}) {{", P(pidx, pneg)));
                indent_++;
            }
            if (so == Op::TEX || so == Op::TEX_b || so == Op::TEXS || so == Op::TLD || so == Op::TLD_b || so == Op::TLDS ||
                so == Op::TLD4 || so == Op::TLD4_b || so == Op::TLD4S || so == Op::TXQ || so == Op::TXQ_b ||
                so == Op::TMML || so == Op::TMML_b)
                TexSample(si, t, so);
            else
                EmitInstruction(t, si, so);
            if (guarded) {
                indent_--;
                Line("}");
            }
        }
        res_.unimplemented.insert("CAL_no_ret");
        return;
    }
    case Op::BAR:
        if (stage_ == Stage::Compute)
            Line("GroupMemoryBarrierWithGroupSync();");
        return;
    case Op::LDS: case Op::STS: {
        // Shared memory: one groupshared array of words. Sizes: 0 U8, 1 S8, 2 U16, 3 S16, 4 32, 5 64, 6 128.
        uses_smem_ = true;
        const s32 off = s32(u32(Bits(insn, 20, 24)) << 8) >> 8;
        const u32 size = u32(Bits(insn, 48, 3));
        const u32 words = size == 5 ? 2 : size == 6 ? 4 : 1;
        Line(fmt::format("tmpu.w = {} + {};", R(a), Hex(u32(off))));
        const std::string w = fmt::format("smem[(tmpu.w >> 2u) % {}u]", SmemWords());
        if (op == Op::LDS) {
            if (size < 4) {
                const u32 bits = size < 2 ? 8 : 16;
                Line(fmt::format("tmpu.z = ({} >> ((tmpu.w & 3u) * 8u)) & {}u;", w, (1u << bits) - 1));
                if (size == 1 || size == 3)
                    Line(fmt::format("tmpu.z = uint(int(tmpu.z << {}u) >> {});", 32 - bits, 32 - bits));
                SetR(d, "tmpu.z");
                return;
            }
            for (u32 i = 0; i < words; i++)
                Line(fmt::format("tmpu4_[{}] = smem[((tmpu.w >> 2u) + {}u) % {}u];", i, i, SmemWords()));
            for (u32 i = 0; i < words; i++)
                SetR(d == RZ ? RZ : d + i, fmt::format("tmpu4_[{}]", i));
        } else {
            if (size < 4) {
                const u32 mask = size < 2 ? 0xFFu : 0xFFFFu;
                Line(fmt::format("tmpu.z = (tmpu.w & 3u) * 8u;"));
                Line(fmt::format("{0} = ({0} & ~({1}u << tmpu.z)) | (({2} & {1}u) << tmpu.z);", w, mask, R(d)));
                return;
            }
            for (u32 i = 0; i < words; i++)
                Line(fmt::format("smem[((tmpu.w >> 2u) + {}u) % {}u] = {};", i, SmemWords(), R(d == RZ ? RZ : d + i)));
        }
        return;
    }
    // Warp operations. Each D3D11 thread runs as a one-lane warp: votes see only their own predicate and a
    // shuffle reads the thread's own value. That keeps the usual warp-aggregated patterns (ballot, elect the
    // first lane, one atomic per warp, broadcast with SHFL.IDX) exact without cross-thread exchange.
    case Op::SHFL: {
        SetR(d, R(a));
        const u32 pd = u32(Bits(insn, 48, 3));
        if (pd != PT)
            Line(fmt::format("{} = true;", P(pd, false)));
        return;
    }
    case Op::ATOMS: {
        uses_smem_ = true;
        const u32 aop = u32(Bits(insn, 52, 4));
        const u32 off = u32(Bits(insn, 30, 22)) << 2;
        Line(fmt::format("tmpu.w = (({} + {}) >> 2u) % {}u;", R(a), Hex(off), SmemWords()));
        Line(fmt::format("tmpu.z = {};", R(u32(Bits(insn, 20, 8)))));
        Line(AtomicOp(aop, "smem[tmpu.w]", "tmpu.z", "tmpu.y"));
        SetR(d, "tmpu.y");
        return;
    }
    case Op::RED: { // reduction: a global atomic without a result. Data 0-7, address 8-15, size 20, op 23, offset 28.
        if (stage_ != Stage::Compute) {
            res_.unimplemented.insert("RED");
            return;
        }
        uses_gmem_ = true;
        const s32 off = s32(u32(Bits(insn, 28, 20)) << 12) >> 12;
        const std::string hi = Bit(insn, 48) && a != RZ ? R(a + 1) : "0u";
        if (off >= 0)
            Line(fmt::format("tmpu.x = {0} + {1}; tmpu.y = {2} + ((tmpu.x < {0}) ? 1u : 0u);", R(a), Hex(u32(off)), hi));
        else
            Line(fmt::format("tmpu.x = {0} - {1}; tmpu.y = {2} - (({0} < {1}) ? 1u : 0u);", R(a), Hex(u32(-off)), hi));
        const u32 size = u32(Bits(insn, 20, 3));
        if (size != 0 && size != 1)
            res_.unimplemented.insert(fmt::format("RED_size{}", size));
        gmem_written_ = true;
        Line(fmt::format("GmemAtomic({}u, tmpu.x, tmpu.y, {});", u32(Bits(insn, 23, 3)), R(d)));
        return;
    }
    case Op::LDG: case Op::STG: case Op::ATOM: {
        if (stage_ != Stage::Compute) { // global memory windows are only bound for compute dispatches
            res_.unimplemented.insert(OpName(op));
            if (op != Op::STG)
                SetR(d, "0u");
            return;
        }
        uses_gmem_ = true;
        const bool wide = op == Op::ATOM ? Bit(insn, 48) : Bit(insn, 45);
        const s32 off = op == Op::ATOM ? s32(u32(Bits(insn, 28, 20)) << 12) >> 12 : s32(u32(Bits(insn, 20, 24)) << 8) >> 8;
        // 64-bit guest address in (lo, hi); the address pair may be overwritten by a load, so evaluate it first.
        const std::string hi = wide && a != RZ ? R(a + 1) : "0u";
        if (off >= 0)
            Line(fmt::format("tmpu.x = {0} + {1}; tmpu.y = {2} + ((tmpu.x < {0}) ? 1u : 0u);", R(a), Hex(u32(off)), hi));
        else
            Line(fmt::format("tmpu.x = {0} - {1}; tmpu.y = {2} - (({0} < {1}) ? 1u : 0u);", R(a), Hex(u32(-off)), hi));
        if (op == Op::LDG || op == Op::STG) {
            const u32 size = u32(Bits(insn, 48, 3));
            const u32 words = size == 5 ? 2 : size == 6 ? 4 : 1;
            if (size < 4)
                res_.unimplemented.insert(fmt::format("{}_subword", OpName(op)));
            if (op == Op::LDG) {
                for (u32 i = 0; i < words; i++)
                    Line(fmt::format("tmpu4_[{}] = GmemLoad(tmpu.x + {}u, tmpu.y);", i, i * 4));
                for (u32 i = 0; i < words; i++)
                    SetR(d == RZ ? RZ : d + i, fmt::format("tmpu4_[{}]", i));
            } else {
                gmem_written_ = true;
                for (u32 i = 0; i < words; i++)
                    Line(fmt::format("GmemStore(tmpu.x + {}u, tmpu.y, {});", i * 4, R(d == RZ ? RZ : d + i)));
            }
            return;
        }
        const u32 aop = u32(Bits(insn, 52, 4));
        const u32 asize = u32(Bits(insn, 49, 3));
        if (asize != 0 && asize != 1)
            res_.unimplemented.insert(fmt::format("{}_size{}", OpName(op), asize));
        gmem_written_ = true;
        Line(fmt::format("tmpu.z = GmemAtomic({}u, tmpu.x, tmpu.y, {});", aop, R(u32(Bits(insn, 20, 8)))));
        if (op == Op::ATOM)
            SetR(d, "tmpu.z");
        return;
    }
    case Op::KIL:
        if (stage_ == Stage::Fragment) {
            const u32 t = u32(Bits(insn, 0, 5));
            if (t == 15)
                Line("discard;");
            else
                Line(fmt::format("if ({}) discard;", CcTest(t)));
        }
        return;
    case Op::VOTE: { // one-lane warp: ALL/ANY are the predicate itself, EQ always holds, the ballot is bit 0
        const std::string pv = P(u32(Bits(insn, 39, 3)), Bit(insn, 42));
        const u32 vop = u32(Bits(insn, 48, 2));
        Line(fmt::format("tmpb = {};", pv));
        SetR(d, "(tmpb ? 1u : 0u)");
        const u32 pd = u32(Bits(insn, 45, 3));
        if (pd != PT)
            Line(fmt::format("{} = {};", P(pd, false), vop == 2 ? "true" : "tmpb"));
        return;
    }
    case Op::FSWZADD:
    case Op::VOTE_vtg:
        res_.unimplemented.insert(OpName(op));
        SetR(d, "0u");
        return;
    // ---------------- packed half precision (two f16 lanes per register)
    case Op::HADD2_reg: case Op::HADD2_cbuf: case Op::HADD2_imm: case Op::HADD2_32I:
    case Op::HMUL2_reg: case Op::HMUL2_cbuf: case Op::HMUL2_imm: case Op::HMUL2_32I:
    case Op::HFMA2_reg: case Op::HFMA2_rc: case Op::HFMA2_cr: case Op::HFMA2_imm: case Op::HFMA2_32I: {
        // Swizzle: 0 H1_H0, 1 F32, 2 H0_H0, 3 H1_H1
        auto half_op = [](const std::string& v, u32 sw) -> std::string {
            switch (sw) {
            case 1: return fmt::format("asfloat({}).xx", v);
            case 2: return fmt::format("H2({}).xx", v);
            case 3: return fmt::format("H2({}).yy", v);
            default: return fmt::format("H2({})", v);
            }
        };
        auto absneg = [](std::string v, bool abs, bool neg) {
            if (abs) v = "abs(" + v + ")";
            if (neg) v = "(-" + v + ")";
            return v;
        };
        auto imm_halves = [](u64 i) {
            return Hex(u32(Bits(i, 20, 9) << 6) | (u32(Bit(i, 29)) << 15) | (u32(Bits(i, 30, 9)) << 22) |
                       (u32(Bit(i, 56)) << 31));
        };
        const bool is32 = op == Op::HADD2_32I || op == Op::HMUL2_32I || op == Op::HFMA2_32I;
        const u32 merge = is32 ? 0 : u32(Bits(insn, 49, 2));
        const u32 swa = is32 ? u32(Bits(insn, 53, 2)) : u32(Bits(insn, 47, 2));
        std::string av = half_op(R(a), swa);
        std::string bv, cv, result;
        bool sat = false;
        const bool is_fma = op == Op::HFMA2_reg || op == Op::HFMA2_rc || op == Op::HFMA2_cr || op == Op::HFMA2_imm ||
                            op == Op::HFMA2_32I;
        // HMUL2_reg has no A negate and HMUL2_cbuf no B negate (those bits mean something else there).
        if (!is_fma && !is32)
            av = absneg(av, Bit(insn, 44), op != Op::HMUL2_reg && Bit(insn, 43));
        switch (op) {
        case Op::HADD2_reg: case Op::HMUL2_reg:
            bv = absneg(half_op(R(u32(Bits(insn, 20, 8))), u32(Bits(insn, 28, 2))), Bit(insn, 30), Bit(insn, 31));
            sat = Bit(insn, 32);
            break;
        case Op::HADD2_cbuf: case Op::HMUL2_cbuf:
            bv = absneg(half_op(CbufOperand(insn), 1), Bit(insn, 54), op == Op::HADD2_cbuf && Bit(insn, 56));
            sat = Bit(insn, 52);
            break;
        case Op::HADD2_imm: case Op::HMUL2_imm:
            bv = half_op(imm_halves(insn), 0);
            sat = Bit(insn, 52);
            break;
        case Op::HADD2_32I:
            if (Bit(insn, 56)) av = "(-" + av + ")";
            bv = half_op(Imm32(insn), 0);
            sat = Bit(insn, 52);
            break;
        case Op::HMUL2_32I:
            bv = half_op(Imm32(insn), 0);
            sat = Bit(insn, 52);
            break;
        case Op::HFMA2_reg:
            bv = absneg(half_op(R(u32(Bits(insn, 20, 8))), u32(Bits(insn, 28, 2))), false, Bit(insn, 31));
            cv = absneg(half_op(R(u32(Bits(insn, 39, 8))), u32(Bits(insn, 35, 2))), false, Bit(insn, 30));
            sat = Bit(insn, 32);
            break;
        case Op::HFMA2_rc:
            bv = absneg(half_op(R(u32(Bits(insn, 39, 8))), u32(Bits(insn, 53, 2))), false, Bit(insn, 56));
            cv = absneg(half_op(CbufOperand(insn), 1), false, Bit(insn, 51));
            sat = Bit(insn, 52);
            break;
        case Op::HFMA2_cr:
            bv = absneg(half_op(CbufOperand(insn), 1), false, Bit(insn, 56));
            cv = absneg(half_op(R(u32(Bits(insn, 39, 8))), u32(Bits(insn, 53, 2))), false, Bit(insn, 51));
            sat = Bit(insn, 52);
            break;
        case Op::HFMA2_imm:
            bv = half_op(imm_halves(insn), 0);
            cv = absneg(half_op(R(u32(Bits(insn, 39, 8))), u32(Bits(insn, 53, 2))), false, Bit(insn, 51));
            sat = Bit(insn, 52);
            break;
        case Op::HFMA2_32I:
            bv = half_op(Imm32(insn), 0);
            cv = absneg(half_op(R(d), 0), false, Bit(insn, 52));
            break;
        default: break;
        }
        const bool is_add = op == Op::HADD2_reg || op == Op::HADD2_cbuf || op == Op::HADD2_imm || op == Op::HADD2_32I;
        if (is_fma)
            result = fmt::format("mad({}, {}, {})", av, bv, cv);
        else if (is_add)
            result = fmt::format("({} + {})", av, bv);
        else
            result = fmt::format("({} * {})", av, bv);
        Line(fmt::format("tmp4.xy = {};", Sat(result, sat)));
        switch (merge) {
        case 1: SetR(d, "asuint(tmp4.x)"); break;
        case 2: SetR(d, fmt::format("({} & 0xFFFF0000u) | f32tof16(tmp4.x)", R(d))); break;
        case 3: SetR(d, fmt::format("({} & 0xFFFFu) | (f32tof16(tmp4.y) << 16)", R(d))); break;
        default: SetR(d, "P2(tmp4.xy)"); break;
        }
        return;
    }
    case Op::HSETP2_reg: case Op::HSETP2_cbuf: case Op::HSETP2_imm: {
        auto half_op = [](const std::string& v, u32 sw) -> std::string {
            switch (sw) {
            case 1: return fmt::format("asfloat({}).xx", v);
            case 2: return fmt::format("H2({}).xx", v);
            case 3: return fmt::format("H2({}).yy", v);
            default: return fmt::format("H2({})", v);
            }
        };
        std::string av = half_op(R(a), u32(Bits(insn, 47, 2)));
        if (Bit(insn, 44)) av = "abs(" + av + ")";
        if (Bit(insn, 43)) av = "(-" + av + ")";
        std::string bv;
        u32 cmp;
        bool h_and;
        if (op == Op::HSETP2_reg) {
            bv = half_op(R(u32(Bits(insn, 20, 8))), u32(Bits(insn, 28, 2)));
            if (Bit(insn, 30)) bv = "abs(" + bv + ")";
            if (Bit(insn, 31)) bv = "(-" + bv + ")";
            cmp = u32(Bits(insn, 35, 4));
            h_and = Bit(insn, 49);
        } else if (op == Op::HSETP2_cbuf) {
            bv = half_op(CbufOperand(insn), 1);
            if (Bit(insn, 54)) bv = "abs(" + bv + ")";
            if (Bit(insn, 56)) bv = "(-" + bv + ")";
            cmp = u32(Bits(insn, 49, 4));
            h_and = Bit(insn, 53);
        } else {
            bv = fmt::format("H2({})", Hex(u32(Bits(insn, 20, 9) << 6) | (u32(Bit(insn, 29)) << 15) |
                                           (u32(Bits(insn, 30, 9)) << 22) | (u32(Bit(insn, 56)) << 31)));
            cmp = u32(Bits(insn, 49, 4));
            h_and = Bit(insn, 53);
        }
        Line(fmt::format("tmp4.xy = {}; tmp4.zw = {};", av, bv));
        const std::string c0 = FCompare(cmp, "tmp4.x", "tmp4.z"), c1 = FCompare(cmp, "tmp4.y", "tmp4.w");
        const std::string sp = P(u32(Bits(insn, 39, 3)), Bit(insn, 42));
        const u32 bop = u32(Bits(insn, 45, 2));
        if (h_and) {
            Line(fmt::format("tmpb = {} && {};", c0, c1));
            SetP(u32(Bits(insn, 3, 3)), Bop(bop, "tmpb", sp));
            SetP(u32(Bits(insn, 0, 3)), Bop(bop, "!tmpb", sp));
        } else {
            SetP(u32(Bits(insn, 3, 3)), Bop(bop, c0, sp));
            SetP(u32(Bits(insn, 0, 3)), Bop(bop, c1, sp));
        }
        return;
    }
    default:
        Unimpl(op, pc, insn);
        return;
    }
}

void Translator::EmitExit() {
    if (stage_ == Stage::Fragment) {
        // Colour outputs live in consecutive registers per the SPH output map.
        const u32 omap = sph_[18];
        u32 reg = 0;
        for (u32 rt = 0; rt < 8; rt++) {
            const u32 m = (omap >> (rt * 4)) & 0xF;
            if (!m)
                continue;
            res_.rt_mask |= 1u << rt;
            static const char* cs = "xyzw";
            for (u32 c = 0; c < 4; c++)
                if (m & (1u << c))
                    Line(fmt::format("out_color{}.{} = {};", rt, cs[c], F(reg++)));
        }
        if (sph_[19] & 2) {
            res_.writes_depth = true;
            Line(fmt::format("out_depth = {};", F(reg + 1)));
        }
    }
    Line("return_flag = true;");
    Line("return;");
}

void Translator::EmitPrologue(std::string& head) {
    // Shared helpers
    head += R"(
#pragma warning(disable : 3557 3550 4000 3570 4121 3078 3206)
cbuffer Driver : register(b13) {
    float4 drv_viewport;     // x: y flip (1/-1), y: depth scale, z: depth bias, w: render scale of the bound targets
    uint4 drv_attrib[32];    // x: format register, y: unused
    uint4 drv_vbuf[16];      // x: stride, y: divisor, z: byte offset in vball (~0 = disabled), w: first element uploaded
    uint4 tex_swizzle[32];   // x: packed swizzle, y: resolution scale of a render-target texture (0/1: none)
    uint4 drv_misc;          // x: base instance, y: alpha test func, z: alpha ref (float bits), w: alpha test enable
    uint4 drv_gmem[8];       // compute global memory windows: x base lo, y base hi, z size
    uint4 drv_image[8];      // compute images (SUST): base, size and layout (see ImageStore)
};
// Resolution scaling: render targets may be stored larger than the guest sees them, by s/3 (tex_swizzle.y in
// thirds: 3 = 1x, 4 = 4/3, 6 = 2x). Texel fetches scale their coordinates and keep the pixel's position inside the
// guest texel (rs_frac, set from SV_Position).
static float2 rs_frac = float2(0, 0);
int2 RsXY(int2 c, uint slot) {
    const uint s = tex_swizzle[slot].y;
    return s > 3u ? int2(floor((float2(c) + rs_frac) * (float(s) / 3.0f))) : c;
}
float2 H2(uint v) { return float2(f16tof32(v & 0xFFFFu), f16tof32(v >> 16)); }
uint P2(float2 v) { return f32tof16(v.x) | (f32tof16(v.y) << 16); }
float4 ApplySwizzle(float4 v, uint s) {
    if (s == 0xB1Au) return v; // identity: R,G,B,A sources (2,3,4,5) packed 3 bits each
    float4 r;
    [unroll] for (uint i = 0u; i < 4u; i++) {
        uint c = (s >> (i * 3u)) & 7u;
        float x = c == 2u ? v.x : c == 3u ? v.y : c == 4u ? v.z : c == 5u ? v.w : (c >= 6u ? 1.0f : 0.0f);
        r[i] = x;
    }
    return r;
}
)";
    // Constant buffers
    for (u32 i = 0; i < 13; i++) {
        if (!(res_.cbuf_mask & (1u << i)))
            continue;
        // Static reads index a cbuffer sized to what the shader uses; dynamic reads go through a raw view of the
        // same data (FXC can spend minutes analysing dynamic indexing of a large cbuffer array inside loops).
        head += fmt::format("cbuffer CB{0} : register(b{0}) {{ uint4 c{0}[{1}]; }};\n", i, std::max<u32>(cbuf_rows_[i], 1));
        if (res_.cbuf_dyn_mask & (1u << i)) {
            head += fmt::format("ByteAddressBuffer cbd{0} : register(t{1});\n", i, 32 + i);
            head += fmt::format("uint CbufLoad{0}(uint a) {{ return cbd{0}.Load(a & 0xFFFCu); }}\n", i);
        }
    }
    // Textures. D3D11 has only 16 sampler registers: with at most 16 textures each gets its own (in use order),
    // otherwise textures past slot 15 share s15. Sampler names stay per texture slot and map onto registers.
    const bool own_samplers = res_.textures.size() <= 16;
    // Past 16 textures, a shadow texture never shares s15: one register cannot hold both a comparison and a plain
    // sampler state (FXC rejects the pair, and the renderer binds only one of them). It takes a register no texture
    // uses, else the highest one a plain texture holds (that texture moves to the shared s15).
    std::vector<u32> sreg_of(res_.textures.size());
    if (!own_samplers) {
        std::array<int, 16> owner;
        owner.fill(-1);
        for (size_t ti = 0; ti < res_.textures.size(); ti++) {
            sreg_of[ti] = std::min<u32>(res_.textures[ti].slot, 15);
            if (sreg_of[ti] < 15)
                owner[sreg_of[ti]] = int(ti);
        }
        for (size_t ti = 0; ti < res_.textures.size(); ti++) {
            if (!res_.textures[ti].shadow || sreg_of[ti] < 15)
                continue;
            int r = 14;
            while (r >= 0 && owner[r] >= 0)
                r--;
            if (r < 0) { // all taken: the highest register held by a plain texture
                for (r = 14; r >= 0 && res_.textures[owner[r]].shadow; r--) {
                }
                if (r < 0)
                    continue; // (15 shadow textures already: stays on s15)
                sreg_of[owner[r]] = 15;
            }
            owner[r] = int(ti);
            sreg_of[ti] = u32(r);
        }
    }
    for (size_t ti = 0; ti < res_.textures.size(); ti++) {
        auto& t = res_.textures[ti];
        const char* type = "Texture2D";
        switch (t.dim) {
        case TexDim::Tex1D: type = t.array ? "Texture1DArray" : "Texture1D"; break;
        case TexDim::Tex2D: type = t.array ? "Texture2DArray" : "Texture2D"; break;
        case TexDim::Tex3D: type = "Texture3D"; break;
        case TexDim::Cube: type = t.array ? "TextureCubeArray" : "TextureCube"; break;
        case TexDim::Buffer: type = "Buffer"; break;
        }
        head += fmt::format("{}<float4> tex{} : register(t{});\n", type, t.slot, t.slot);
        const u32 sreg = own_samplers ? u32(ti) : sreg_of[ti];
        t.sampler = sreg;
        if (declared_smp_.insert(sreg).second)
            head += fmt::format("SamplerState smp_r{} : register(s{});\n", sreg, sreg);
        if (t.shadow && declared_smpc_.insert(sreg).second)
            head += fmt::format("SamplerComparisonState smpc_r{} : register(s{});\n", sreg, sreg);
        head += fmt::format("#define smp{} smp_r{}\n", t.slot, sreg);
        if (t.shadow)
            head += fmt::format("#define smpc{} smpc_r{}\n", t.slot, sreg);
    }
}

bool Translator::BuildStructuredBlocks() {
    sblocks_.assign(blocks_.size(), {});
    for (auto& [start, b] : blocks_) {
        cur_block_ = start;
        out_.clear();
        indent_ = 0;
        SBlock& sb = sblocks_[b.id];
        bool has_term = false;
        for (u32 pc = b.start; pc < b.end; pc = NextPc(pc)) {
            const u64 insn = At(pc);
            const Op op = Decode(insn);
            const u32 pidx = u32(Bits(insn, 16, 3));
            const bool pneg = Bit(insn, 19);
            const bool guarded = !(pidx == PT && !pneg);
            const std::string guard = P(pidx, pneg);
            const bool is_flow = op == Op::BRA || op == Op::EXIT || op == Op::SYNC || op == Op::BRK || op == Op::CONT;
            if (op == Op::BRX) {
                if (NextPc(pc) != b.end || guarded)
                    return false;
                Term& t = sb.term;
                t.kind = Term::Switch;
                t.sel = R(u32(Bits(insn, 8, 8)));
                for (auto& [value, target] : brx_[pc]) {
                    auto it = blocks_.find(target);
                    if (it == blocks_.end())
                        return false;
                    t.cases.push_back({value, it->second.id});
                }
                has_term = true;
                continue;
            }
            if (is_flow) {
                if (NextPc(pc) != b.end)
                    return false; // flow in the middle of a block
                std::string cond = guarded ? guard : "";
                const u32 cct = u32(Bits(insn, 0, 5));
                if (cct != 15) {
                    const std::string t = CcTest(cct);
                    cond = cond.empty() ? t : "(" + cond + " && " + t + ")";
                }
                Term& t = sb.term;
                if (op == Op::EXIT) {
                    std::string saved;
                    saved.swap(out_);
                    EmitExit();
                    t.exit_code.swap(out_);
                    out_.swap(saved);
                    t.kind = cond.empty() ? Term::Exit : Term::CondExit;
                } else {
                    const u32 target = op == Op::BRA ? BranchTarget(pc, insn)
                                                     : (sync_target_.count(pc) ? sync_target_[pc] : 0xFFFFFFFFu);
                    auto it = blocks_.find(target);
                    t.target = it == blocks_.end() ? ~0u : it->second.id;
                    t.kind = cond.empty() ? Term::Jump : Term::Cond;
                }
                t.cond = cond;
                has_term = true;
                continue;
            }
            if (guarded) {
                Line(fmt::format("if ({}) {{", guard));
                indent_++;
            }
            if (op == Op::TEX || op == Op::TEX_b || op == Op::TEXS || op == Op::TLD || op == Op::TLD_b ||
                op == Op::TLDS || op == Op::TLD4 || op == Op::TLD4_b || op == Op::TLD4S || op == Op::TXQ ||
                op == Op::TXQ_b || op == Op::TMML || op == Op::TMML_b)
                TexSample(insn, pc, op);
            else
                EmitInstruction(pc, insn, op);
            if (guarded) {
                indent_--;
                Line("}");
            }
        }
        if (!has_term) {
            auto next = blocks_.find(b.end);
            sb.term.kind = Term::Fall;
            sb.term.target = next == blocks_.end() ? ~0u : next->second.id;
        }
        sb.body.swap(out_);
    }
    return true;
}

void Translator::SText(const std::string& text) {
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos)
            e = text.size();
        if (e > p)
            sout_.append(size_t(sindent_) * 4, ' ').append(text, p, e - p).append("\n");
        p = e + 1;
    }
}

// A jump out of block `from`: continue/break/return, or falling out of the region it ends.
bool Translator::Goto(u32 from, u32 target, u32 last, u32 end_target, LoopCtx ctx) {
    if (target == ~0u) {
        SLine("return_flag = true; return; // jump outside program");
        return true;
    }
    if (target == ctx.header) {
        SLine("continue;");
        return true;
    }
    if (target == ctx.exit) {
        SLine("break;");
        return true;
    }
    return from + 1 == last && target == end_target;
}

bool Translator::Seq(u32 first, u32 last, u32 end_target, LoopCtx ctx, u32 skip_header) {
    const u32 n = u32(sblocks_.size());
    u32 i = first;
    while (i < last) {
        // A flagged forward jump from an earlier region lands inside this one: skip to its target.
        if (auto f = std::find_if(pending_fwd_.begin(), pending_fwd_.end(), [&](u32 t) { return t > i && t <= last; });
            f != pending_fwd_.end()) {
            const u32 t = *f;
            pending_fwd_.erase(f);
            SLine(fmt::format("if (!fwd{}) {{", t));
            sindent_++;
            if (!Seq(i, t, t, ctx, ~0u))
                return false;
            sindent_--;
            SLine("}");
            SLine(fmt::format("fwd{} = false;", t));
            i = t;
            continue;
        }
        // A flagged forward jump escapes this region altogether: the rest of the region must not run either, and
        // the flag stays pending for the enclosing region that contains its target.
        if (auto f = std::find_if(pending_fwd_.begin(), pending_fwd_.end(), [&](u32 t) { return t > last; });
            f != pending_fwd_.end()) {
            const u32 t = *f;
            pending_fwd_.erase(f);
            SLine(fmt::format("if (!fwd{}) {{", t));
            sindent_++;
            if (!Seq(i, last, end_target, ctx, skip_header))
                return false;
            sindent_--;
            SLine("}");
            pending_fwd_.insert(t);
            i = last;
            continue;
        }
        // Loop header: the latest back edge into i closes the loop.
        if (i != skip_header) {
            u32 latch = ~0u;
            for (u32 j = i; j < n; j++) {
                const Term& t = sblocks_[j].term;
                if ((t.kind == Term::Jump || t.kind == Term::Cond) && t.target == i)
                    latch = j;
            }
            if (latch != ~0u) {
                const u32 loop_end = latch + 1;
                if (loop_end > last)
                    return SFail(fmt::format("loop {}..{} crosses region end {}", i, latch, last));
                SLine("[loop] for (uint li_ = 0u; ; li_++) {");
                sindent_++;
                SLine("if (li_ >= 4096u) { loop_capped = true; break; }");
                if (!Seq(i, loop_end, loop_end, LoopCtx{i, loop_end}, i))
                    return false;
                SLine("break;");
                sindent_--;
                SLine("}");
                i = loop_end;
                continue;
            }
        }
        const SBlock& b = sblocks_[i];
        SText(b.body);
        const Term& t = b.term;
        switch (t.kind) {
        case Term::Exit:
            SText(t.exit_code);
            i++;
            continue;
        case Term::CondExit:
            SLine(fmt::format("if ({}) {{", t.cond));
            sindent_++;
            SText(t.exit_code);
            sindent_--;
            SLine("}");
            i++;
            continue;
        case Term::Switch: {
            // Cases are the contiguous regions between the distinct targets; the highest target is the join.
            std::vector<u32> ids;
            for (auto& c : t.cases)
                ids.push_back(c.second);
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            const u32 join = ids.back();
            if (ids.front() <= i || join > last)
                return SFail(fmt::format("switch at {} targets {}..{} outside region {}..{}", i, ids.front(), join, first, last));
            SLine(fmt::format("uint sw{} = {};", i, t.sel));
            for (size_t k = 0; k + 1 < ids.size(); k++) {
                std::string cond;
                for (auto& c : t.cases)
                    if (c.second == ids[k])
                        cond += fmt::format("{}sw{} == {}", cond.empty() ? "" : " || ", i, Hex(c.first));
                SLine(fmt::format("{}if ({}) {{", k ? "} else " : "", cond));
                sindent_++;
                if (!Seq(ids[k], ids[k + 1], join, ctx, ~0u))
                    return false;
                sindent_--;
            }
            if (ids.size() > 1)
                SLine("}");
            i = join;
            continue;
        }
        case Term::Fall:
        case Term::Jump:
            if (t.target == i + 1 && (i + 1 < last || t.target == end_target)) {
                i++;
                continue;
            }
            if (t.target != ~0u && t.target > last && t.target != ctx.header && t.target != ctx.exit &&
                (ctx.exit == ~0u || t.target < ctx.exit)) {
                SLine(fmt::format("fwd{} = true;", t.target));
                pending_fwd_.insert(t.target);
                fwd_used_.insert(t.target);
                if (i + 1 < last) {
                    SLine(fmt::format("if (!fwd{}) {{", t.target));
                    sindent_++;
                    if (!Seq(i + 1, last, end_target, ctx, ~0u))
                        return false;
                    sindent_--;
                    SLine("}");
                }
                i = last;
                continue;
            }
            if (!Goto(i, t.target, last, end_target, ctx))
                return SFail(fmt::format("jump {} -> {} (region end {}, join {})", i, t.target, last, end_target));
            i++;
            continue;
        case Term::Cond: {
            const u32 tg = t.target;
            if (tg == ~0u || tg == ctx.header || tg == ctx.exit) {
                SLine(fmt::format("if ({}) {{", t.cond));
                sindent_++;
                Goto(i, tg, last, end_target, ctx);
                sindent_--;
                SLine("}");
                // Not taken: fall through to the next block.
                if (i + 1 >= last && end_target != i + 1 && !Goto(i, i + 1, last, end_target, ctx))
                    return SFail(fmt::format("fallthrough out of region at {}", i));
                i++;
                continue;
            }
            if (tg > last && tg != ~0u && (ctx.exit == ~0u || tg < ctx.exit)) {
                // Forward jump out of this region: remember it in a flag, skip the rest of the region, and let
                // the enclosing sequence skip everything up to the target.
                SLine(fmt::format("fwd{} = {};", tg, t.cond));
                pending_fwd_.insert(tg);
                fwd_used_.insert(tg);
                if (i + 1 < last) {
                    SLine(fmt::format("if (!fwd{}) {{", tg));
                    sindent_++;
                    if (!Seq(i + 1, last, end_target, ctx, ~0u))
                        return false;
                    sindent_--;
                    SLine("}");
                }
                i = last;
                continue;
            }
            if (tg <= i + 1 || tg > last)
                return SFail(fmt::format("cond {} -> {} (region {}..{})", i, tg, first, last));
            // if-else: the then-part ends with a jump over the else-part to a join point.
            const Term& tl = sblocks_[tg - 1].term;
            if (tl.kind == Term::Jump && tl.target != ~0u && tl.target > tg && tl.target <= last &&
                tl.target != ctx.exit) {
                const u32 join = tl.target;
                SLine(fmt::format("if (!({})) {{", t.cond));
                sindent_++;
                if (!Seq(i + 1, tg, join, ctx, ~0u))
                    return false;
                sindent_--;
                SLine("} else {");
                sindent_++;
                if (!Seq(tg, join, join, ctx, ~0u))
                    return false;
                sindent_--;
                SLine("}");
                i = join;
                continue;
            }
            SLine(fmt::format("if (!({})) {{", t.cond));
            sindent_++;
            if (!Seq(i + 1, tg, tg, ctx, ~0u))
                return false;
            sindent_--;
            SLine("}");
            i = tg;
            continue;
        }
        }
    }
    return true;
}

bool Translator::EmitStructured(std::string& body) {
    if (std::getenv("MUA3_NO_STRUCTURED"))
        return false;
    if (!BuildStructuredBlocks())
        return false;
    sout_.clear();
    sindent_ = 1;
    pending_fwd_.clear();
    fwd_used_.clear();
    if (!Seq(0, u32(sblocks_.size()), ~0u, LoopCtx{}, ~0u)) {
        if (std::getenv("MUA3_STRUCT_DEBUG")) {
            std::string blocks;
            for (u32 i = 0; i < sblocks_.size(); i++)
                blocks += fmt::format(" {}:k{}->{}", i, int(sblocks_[i].term.kind), sblocks_[i].term.target == ~0u ? -1 : int(sblocks_[i].term.target));
            fmt::print(stderr, "structurize failed: {}\n blocks:{}\n", sfail_, blocks);
        }
        return false;
    }
    SLine("return_flag = true;");
    body = std::move(sout_);
    return true;
}

bool Translator::Run() {
    if (!DiscoverBlocks())
        return false;
    AnalyzeStacks();
    res_.uses_loop = has_back_edge_;

    // Body
    std::string body;
    const bool structured = EmitStructured(body);
    res_.structured = structured;
    if (!structured) {
    out_.clear();
    indent_ = 1;
    // Only the blocks between the earliest back-edge target and the latest back-edge source need the dispatch
    // loop; keeping the rest outside keeps FXC (and the driver compiler) fast on shaders with small loops.
    u32 loop_lo = ~0u, loop_hi = 0;
    if (has_back_edge_) {
        for (auto& [start, b] : blocks_) {
            for (u32 pc = b.start; pc < b.end; pc = NextPc(pc)) {
                const u64 insn = At(pc);
                const Op op = Decode(insn);
                std::vector<u32> targets;
                if (op == Op::BRA)
                    targets.push_back(BranchTarget(pc, insn));
                else if ((op == Op::SYNC || op == Op::BRK || op == Op::CONT) && sync_target_.count(pc))
                    targets.push_back(sync_target_[pc]);
                else if (op == Op::BRX && brx_.count(pc)) // every jump table entry (some go backwards)
                    for (auto& [value, target] : brx_[pc])
                        targets.push_back(target);
                for (const u32 target : targets) {
                    auto t = blocks_.find(target);
                    if (t != blocks_.end() && t->second.id <= b.id) {
                        loop_lo = std::min(loop_lo, t->second.id);
                        loop_hi = std::max(loop_hi, b.id);
                    }
                }
            }
        }
    }
    Line("uint blk = 0u;");
    for (auto& [start, b] : blocks_) {
        if (b.id == loop_lo) {
            // Each iteration dispatches one basic block; cap runaway loops so a bad translation cannot hang the GPU.
            Line("[loop] for (uint iter_ = 0u; ; iter_++) {");
            indent_++;
            Line("if (iter_ >= 1024u) { loop_capped = true; break; }");
        }
        cur_block_ = start;
        Line(fmt::format("[branch] if (blk == {}u) {{ // block {:#x}", b.id, start));
        indent_++;
        bool terminated = false;
        for (u32 pc = b.start; pc < b.end; pc = NextPc(pc)) {
            const u64 insn = At(pc);
            const Op op = Decode(insn);
            const u32 pidx = u32(Bits(insn, 16, 3));
            const bool pneg = Bit(insn, 19);
            const bool guarded = !(pidx == PT && !pneg);
            const std::string guard = P(pidx, pneg);
            const bool is_flow = op == Op::BRA || op == Op::EXIT || op == Op::SYNC || op == Op::BRK || op == Op::CONT;
            if (op == Op::BRX) {
                if (guarded) {
                    Line(fmt::format("if ({}) {{", guard));
                    indent_++;
                }
                Line(fmt::format("tmpu.w = {};", R(u32(Bits(insn, 8, 8)))));
                bool first = true;
                for (auto& [value, target] : brx_[pc]) {
                    Line(fmt::format("{}if (tmpu.w == {}) {{", first ? "" : "else ", Hex(value)));
                    indent_++;
                    EmitJump(target);
                    indent_--;
                    Line("}");
                    first = false;
                }
                if (guarded) {
                    indent_--;
                    auto next = blocks_.find(b.end);
                    Line("} else {");
                    if (next != blocks_.end())
                        Line(fmt::format("    blk = {}u;", next->second.id));
                    Line("}");
                }
                terminated = true;
                continue;
            }
            if (is_flow) {
                std::string cond = guarded ? guard : "";
                const u32 cct = u32(Bits(insn, 0, 5));
                if (cct != 15) {
                    const std::string t = CcTest(cct);
                    cond = cond.empty() ? t : "(" + cond + " && " + t + ")";
                }
                u32 target = 0;
                if (op == Op::BRA)
                    target = BranchTarget(pc, insn);
                else if (op != Op::EXIT)
                    target = sync_target_.count(pc) ? sync_target_[pc] : 0xFFFFFFFF;
                if (!cond.empty()) {
                    Line(fmt::format("if ({}) {{", cond));
                    indent_++;
                }
                if (op == Op::EXIT)
                    EmitExit();
                else
                    EmitJump(target);
                if (!cond.empty()) {
                    indent_--;
                    // A conditional jump ending the block: the fall-through edge must only run when not taken.
                    auto next = blocks_.find(b.end);
                    if (NextPc(pc) == b.end && next != blocks_.end()) {
                        Line("} else {");
                        indent_++;
                        Line(fmt::format("blk = {}u;", next->second.id));
                        indent_--;
                        terminated = true;
                    }
                    Line("}");
                } else {
                    terminated = true;
                }
                continue;
            }
            if (guarded) {
                Line(fmt::format("if ({}) {{", guard));
                indent_++;
            }
            if (op == Op::TEX || op == Op::TEX_b || op == Op::TEXS || op == Op::TLD || op == Op::TLD_b ||
                op == Op::TLDS || op == Op::TLD4 || op == Op::TLD4_b || op == Op::TLD4S || op == Op::TXQ ||
                op == Op::TXQ_b || op == Op::TMML || op == Op::TMML_b)
                TexSample(insn, pc, op);
            else
                EmitInstruction(pc, insn, op);
            if (guarded) {
                indent_--;
                Line("}");
            }
        }
        if (!terminated) {
            // fall through to the next block
            auto next = blocks_.find(b.end);
            if (next != blocks_.end())
                Line(fmt::format("blk = {}u;", next->second.id));
            else
                Line("return_flag = true; return;");
        }
        indent_--;
        Line("}");
        if (b.id == loop_hi && loop_lo != ~0u) {
            Line("break; // no back edge taken: leave the loop");
            indent_--;
            Line("}");
        }
    }
    body = std::move(out_);
    }

    // Assemble
    std::string src;
    EmitPrologue(src);
    std::string decls;
    decls += "static bool return_flag = false;\nstatic float4 tmp4;\nstatic uint4 tmpu;\nstatic uint tmpu4_[4];\nstatic bool tmpb;\n";
    decls += "static bool loop_capped = false; // a translated loop ran away (pixel shaders then output magenta)\n";
    for (u32 r : regs_used_)
        decls += fmt::format("static uint r{} = 0u;\n", r);
    for (u32 p : preds_used_)
        decls += fmt::format("static bool p{} = false;\n", p);
    if (cc_used_)
        decls += "static bool cc_z = false, cc_s = false, cc_c = false, cc_o = false;\n";
    if (uses_lmem_)
        decls += "static uint lmem[1024];\n";
    for (u32 t : fwd_used_)
        decls += fmt::format("static bool fwd{} = false;\n", t);
    decls += "static uint3 local_id = uint3(0u, 0u, 0u);\nstatic uint3 group_id = uint3(0u, 0u, 0u);\n";

    if (stage_ == Stage::Vertex) {
        src += R"(
// Vertex attributes arrive through the D3D11 input assembler (input layout built per draw).
)";
        src += "struct VSOut {\n    float4 pos : SV_Position;\n";
        for (u32 i = 0; i < kMaxVaryings; i++)
            src += fmt::format("    float4 a{0} : ATTR{0};\n", i);
        src += "};\n";
        src += decls;
        src += "static float4 o_pos = float4(0, 0, 0, 1);\n";
        for (u32 i = 0; i < kMaxVaryings; i++)
            src += fmt::format("static float4 o_attr{} = float4(0, 0, 0, 1);\n", i);
        for (u32 i : vs_inputs_)
            src += fmt::format("static uint4 vin{};\n", i);
        src += "static uint vertex_id, instance_id;\n";
        src += "void Run() {\n" + body + "}\n";
        src += "struct VSIn {\n    uint vid : SV_VertexID;\n    uint iid : SV_InstanceID;\n";
        for (u32 i : vs_inputs_)
            src += fmt::format("    float4 a{0} : ATTR{0};\n", i);
        src += "};\n";
        src += "VSOut main(VSIn input) {\n";
        src += "    vertex_id = input.vid; instance_id = input.iid + drv_misc.x;\n";
        for (u32 i : vs_inputs_)
            src += fmt::format("    vin{0} = asuint(input.a{0});\n", i);
        src += "    Run();\n    VSOut o;\n    o.pos = o_pos;\n";
        src += "    o.pos.y *= drv_viewport.x;\n    o.pos.z = o.pos.z * drv_viewport.y + o.pos.w * drv_viewport.z;\n";
        for (u32 i = 0; i < kMaxVaryings; i++)
            src += fmt::format("    o.a{0} = o_attr{0};\n", i);
        src += "    return o;\n}\n";
    } else if (stage_ == Stage::Fragment) {
        src += "struct PSIn {\n    float4 pos : SV_Position;\n";
        const u8* sph8 = reinterpret_cast<const u8*>(sph_);
        for (u32 i = 0; i < kMaxVaryings; i++) {
            const u8 imap = sph8[0x18 + i];
            const u32 mode = imap & 3; // x component
            const char* interp = mode == 1 ? "nointerpolation " : mode == 3 ? "noperspective " : "";
            src += fmt::format("    {}float4 a{} : ATTR{};\n", interp, i, i);
        }
        src += "    bool front : SV_IsFrontFace;\n};\n";
        src += "struct PSOut {\n";
        for (u32 rt = 0; rt < 8; rt++)
            if ((sph_[18] >> (rt * 4)) & 0xF)
                src += fmt::format("    float4 c{0} : SV_Target{0};\n", rt);
        if (sph_[19] & 2)
            src += "    float depth : SV_Depth;\n";
        src += "};\n";
        src += decls;
        for (u32 rt = 0; rt < 8; rt++)
            src += fmt::format("static float4 out_color{} = float4(0, 0, 0, 0);\n", rt);
        src += "static float out_depth = 0.0f;\nstatic float4 frag_coord;\nstatic bool front_face;\n";
        for (u32 i : in_attrs_)
            src += fmt::format("static float4 in_attr{};\n", i);
        src += "void Run() {\n" + body + "}\n";
        src += "PSOut main(PSIn i) {\n";
        src += "    frag_coord = i.pos; frag_coord.w = 1.0f / i.pos.w; front_face = i.front;\n"
               "    if (drv_viewport.w > 1.0f) { frag_coord.xy = i.pos.xy / drv_viewport.w;"
               " rs_frac = frac(frag_coord.xy); }\n";
        for (u32 a : in_attrs_)
            src += fmt::format("    in_attr{0} = i.a{0};\n", a);
        src += "    Run();\n";
        if (has_back_edge_)
            src += "    if (loop_capped) out_color0 = float4(1, 0, 1, 1);\n";
        src += "    if (drv_misc.w != 0u) { float ref = asfloat(drv_misc.z); float a = out_color0.w; uint f = drv_misc.y;\n"
               "        bool at_pass = f == 1u ? false : f == 2u ? a < ref : f == 3u ? a == ref : f == 4u ? a <= ref : f == 5u ? a > ref"
               " : f == 6u ? a != ref : f == 7u ? a >= ref : true; if (!at_pass) discard; }\n";
        src += "    PSOut o;\n";
        for (u32 rt = 0; rt < 8; rt++)
            if ((sph_[18] >> (rt * 4)) & 0xF)
                src += fmt::format("    o.c{0} = out_color{0};\n", rt);
        if (sph_[19] & 2)
            src += "    o.depth = out_depth;\n";
        src += "    return o;\n}\n";
    } else if (stage_ == Stage::Compute) {
        src += decls;
        if (uses_gmem_) {
            res_.uses_gmem = true;
            res_.writes_gmem = gmem_written_;
            // Guest global memory: up to kGmemWindows storage buffers (NVN's c0 table), each a raw UAV whose
            // guest address range is in drv_gmem[i] (x: base lo, y: base hi, z: size in bytes).
            for (u32 i = 0; i < kGmemWindows; i++)
                src += fmt::format("RWByteAddressBuffer gmem{0} : register(u{0});\n", i);
            src += "uint GmemWindow(uint lo, uint hi, out uint off) {\n";
            src += fmt::format("    [unroll] for (uint i = 0u; i < {}u; i++) {{\n", kGmemWindows);
            src += "        uint4 w = drv_gmem[i];\n"
                   "        if (w.z != 0u && hi == w.y && lo >= w.x && lo - w.x < w.z) { off = lo - w.x; return i; }\n"
                   "    }\n    off = 0u;\n    return 0xFFFFFFFFu;\n}\n";
            src += "uint GmemLoad(uint lo, uint hi) {\n    uint off; uint w = GmemWindow(lo, hi, off);\n    switch (w) {\n";
            for (u32 i = 0; i < kGmemWindows; i++)
                src += fmt::format("    case {0}u: return gmem{0}.Load(off & ~3u);\n", i);
            src += "    default: return 0u;\n    }\n}\n";
            src += "void GmemStore(uint lo, uint hi, uint v) {\n    uint off; uint w = GmemWindow(lo, hi, off);\n    switch (w) {\n";
            for (u32 i = 0; i < kGmemWindows; i++)
                src += fmt::format("    case {0}u: gmem{0}.Store(off & ~3u, v); break;\n", i);
            src += "    default: break;\n    }\n}\n";
            src += "uint GmemAtomic(uint op, uint lo, uint hi, uint v) {\n    uint off; uint w = GmemWindow(lo, hi, off); uint o = 0u;\n"
                   "    off &= ~3u;\n    switch (w) {\n";
            for (u32 i = 0; i < kGmemWindows; i++) {
                src += fmt::format("    case {}u:\n", i);
                src += fmt::format("        if (op == 0u) gmem{0}.InterlockedAdd(off, v, o);\n"
                                   "        else if (op == 1u) gmem{0}.InterlockedMin(off, v, o);\n"
                                   "        else if (op == 2u) gmem{0}.InterlockedMax(off, v, o);\n"
                                   "        else if (op == 5u) gmem{0}.InterlockedAnd(off, v, o);\n"
                                   "        else if (op == 6u) gmem{0}.InterlockedOr(off, v, o);\n"
                                   "        else if (op == 7u) gmem{0}.InterlockedXor(off, v, o);\n"
                                   "        else gmem{0}.InterlockedExchange(off, v, o);\n        break;\n", i);
            }
            src += "    default: break;\n    }\n    return o;\n}\n";
            if (!res_.images.empty()) {
                // drv_image[k]: x/y base address, z width | height << 16, w depth | mode << 16 | bh << 24 | bd << 28.
                // Modes: 1 RGBA16F, 2 RGBA32 raw, 3 RGBA8 UNORM, 4 R32 raw, 5 RG16F, 6 RG32 raw, 7 R11G11B10F.
                src += R"HLSL(uint GobOffset(uint x, uint y) {
    return ((x % 64u) / 32u) * 256u + ((y % 8u) / 2u) * 64u + ((x % 32u) / 16u) * 32u + (y % 2u) * 16u + (x % 16u);
}
void ImageStore(uint k, uint x, uint y, uint z, uint4 v, uint mask) {
    uint4 im = drv_image[k];
    uint w = im.z & 0xFFFFu, h = im.z >> 16, d = im.w & 0xFFFFu, mode = (im.w >> 16) & 0xFFu;
    uint bh = (im.w >> 24) & 7u, bd = (im.w >> 28) & 7u;
    if (mode == 0u || x >= w || y >= h || z >= d) return;
    uint bpb = mode == 2u ? 16u : (mode == 1u || mode == 6u) ? 8u : 4u;
    uint gobs_x = (w * bpb + 63u) / 64u, gobs_y = (h + 7u) / 8u;
    uint blocks_y = (gobs_y + (1u << bh) - 1u) >> bh;
    uint block_size = 512u << (bh + bd), rows = 8u << bh;
    uint xb = x * bpb;
    uint off = ((z >> bd) * blocks_y + y / rows) * gobs_x * block_size +
               ((z & ((1u << bd) - 1u)) * (1u << bh) + (y % rows) / 8u) * 512u + (xb / 64u) * block_size + GobOffset(xb, y);
    uint4 t = v;
    uint n = 1u;
    if (mode == 1u) { t.x = f32tof16(asfloat(v.x)) | (f32tof16(asfloat(v.y)) << 16); t.y = f32tof16(asfloat(v.z)) | (f32tof16(asfloat(v.w)) << 16); n = 2u; }
    else if (mode == 2u) n = 4u;
    else if (mode == 3u) { uint4 b = uint4(saturate(asfloat(v)) * 255.0f + 0.5f); t.x = b.x | (b.y << 8) | (b.z << 16) | (b.w << 24); }
    else if (mode == 5u) t.x = f32tof16(asfloat(v.x)) | (f32tof16(asfloat(v.y)) << 16);
    else if (mode == 6u) n = 2u;
    else if (mode == 7u) { // half floats without sign and low mantissa bits: 11, 11 and 10 bits
        uint3 hf = f32tof16(max(asfloat(v.xyz), 0.0f));
        t.x = ((hf.x >> 4) & 0x7FFu) | (((hf.y >> 4) & 0x7FFu) << 11) | (((hf.z >> 5) & 0x3FFu) << 22);
    }
    uint lo = im.x + off, hi = im.y + (lo < im.x ? 1u : 0u);
    GmemStore(lo, hi, t.x);
    if (n > 1u) GmemStore(lo + 4u, hi, t.y);
    if (n > 2u) { GmemStore(lo + 8u, hi, t.z); GmemStore(lo + 12u, hi, t.w); }
}
)HLSL";
            }
        }
        if (uses_smem_)
            src += fmt::format("groupshared uint smem[{}];\n", SmemWords());
        src += "void Run() {\n" + body + "}\n";
        src += fmt::format("[numthreads({}, {}, {})]\n", opt_.block[0], opt_.block[1], opt_.block[2]);
        src += "void main(uint3 ltid : SV_GroupThreadID, uint3 gid : SV_GroupID) {\n"
               "    local_id = ltid; group_id = gid;\n    Run();\n}\n";
    } else {
        res_.error = "unsupported stage";
        return false;
    }
    res_.hlsl = std::move(src);
    res_.ok = true;
    return true;
}

} // namespace

size_t ProgramSize(const u8* code, size_t max_size) {
    constexpr u64 kSelfBranchA = 0xE2400FFFFF87000Full;
    constexpr u64 kSelfBranchB = 0xE2400FFFFF07000Full;
    for (size_t off = kProgramStart + 8; off + 8 <= max_size; off += 8) {
        u64 v;
        std::memcpy(&v, code + off, 8);
        if (v == kSelfBranchA || v == kSelfBranchB)
            return off + 8;
    }
    return max_size;
}

std::vector<BrxSite> FindBrxSites(const u8* code, size_t size) {
    // the same pattern ResolveBrx looks for: LDC Ra, c1[Rx + off] ... IMNMX bound ... BRX Ra + imm
    std::vector<BrxSite> out;
    auto at = [&](u32 pc) {
        u64 v = 0;
        if (pc + 8 <= size)
            std::memcpy(&v, code + pc, 8);
        return v;
    };
    for (u32 pc = kProgramStart + 8; pc + 8 <= size; pc += 8) {
        if ((pc - kProgramStart) % 32 == 0)
            continue;
        const u64 insn = at(pc);
        if (Decode(insn) != Op::BRX)
            continue;
        const u32 ra = u32(Bits(insn, 8, 8));
        const s32 off = s32(u32(Bits(insn, 20, 24)) << 8) >> 8;
        BrxSite s;
        s.pc = pc;
        s.base = u32(s32(pc + 8) + off);
        u32 cb_index = ~0u;
        s32 bound = -1;
        for (u32 q = pc, steps = 0; q >= 16 && steps < 48; steps++) {
            q -= 8;
            if (q % 32 == 0)
                continue;
            const u64 j = at(q);
            const Op o = Decode(j);
            if (cb_index == ~0u) {
                if (o == Op::LDC && u32(Bits(j, 0, 8)) == ra) {
                    cb_index = u32(Bits(j, 36, 5));
                    s.cb_off = u32(s32(u32(Bits(j, 20, 16)) << 16) >> 16);
                }
                continue;
            }
            if (o == Op::IMNMX_imm) {
                bound = s32(u32(Bits(j, 20, 19)) | (Bit(j, 56) ? 0xFFF80000u : 0u));
                break;
            }
        }
        if (cb_index == 1 && bound >= 0 && bound <= 255) {
            s.entries = u32(bound) + 1;
            out.push_back(s);
        }
    }
    return out;
}

bool UsesBrx(const u8* code, size_t size) {
    for (size_t off = kProgramStart; off + 8 <= size; off += 8) {
        if ((off - kProgramStart) % 32 == 0) // scheduling control word
            continue;
        u64 v;
        std::memcpy(&v, code + off, 8);
        if (Decode(v) == Op::BRX)
            return true;
    }
    return false;
}

TranslateResult Translate(Stage stage, const u8* code, size_t size, const TranslateOptions& opt) {
    TranslateResult res;
    if (size < kProgramStart + 16) {
        res.error = "program too small";
        return res;
    }
    Translator t(stage, code, size, opt, res);
    t.Run();
    return res;
}

} // namespace gpu::shader
