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
        static const char* comp = "xyzw";
        return fmt::format("c{}[{}].{}", index, byte_off >> 4, comp[(byte_off >> 2) & 3]);
    }
    std::string CbufDyn(u32 index, const std::string& byte_addr) {
        res_.cbuf_mask |= 1u << index;
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
    void EmitTexWrites(u32 mask, const std::vector<u32>& regs, const std::string& value, u32 slot);
    void TexSample(u64 insn, u32 pc, Op op);

    // ---------------------------------------------------------------- flow
    bool DiscoverBlocks();
    bool AnalyzeStacks();
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
    bool uses_lmem_ = false, uses_frag_coord_ = false, uses_front_face_ = false;
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
                return fmt::format("{}.SampleCmp(smpc{}, {}, {}).xxxx", t, slot, coords, dref);
            return fmt::format("{}.SampleCmpLevelZero(smpc{}, {}, {}).xxxx", t, slot, coords, dref);
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
        case 13: dim = TexDim::Cube; lk = 2; lod = coord(rb);
            coords = fmt::format("float3({}, {}, {})", coord(ra), coord(ra + 1), coord(ra + 2)); break;
        default: Unimpl(op, pc, insn); return;
        }
        const u32 slot = TexSlot(cbuf_off, dim, array, shadow);
        EmitTexWrites(mask, {d0, d0 == RZ ? RZ : d0 + 1, d1, d1 == RZ ? RZ : d1 + 1}, sample(slot, coords, lk, lod, dref),
                      slot);
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
        case 0: dim = TexDim::Tex1D; slot = TexSlot(cbuf_off, dim, false, false);
            load = fmt::format("tex{}.Load(int2(int({}), 0))", slot, R(ra)); break;
        case 1: slot = TexSlot(cbuf_off, dim, false, false);
            load = fmt::format("tex{}.Load(int3(int({}), int({}), 0))", slot, R(ra), R(rb)); break;
        case 2: slot = TexSlot(cbuf_off, dim, false, false);
            load = fmt::format("tex{}.Load(int3(int({}), int({}), int({})))", slot, R(ra), R(ra + 1), R(rb)); break;
        case 4: slot = TexSlot(cbuf_off, dim, false, false);
            load = fmt::format("tex{}.Load(int3(int({}), int({}), 0))", slot, R(ra), R(ra + 1)); break;
        case 8: slot = TexSlot(cbuf_off, dim, false, false);
            load = fmt::format("tex{}.Load(int3(int({}), int({}), 0))", slot, R(ra), R(ra + 1)); break;
        case 10: dim = TexDim::Tex3D; slot = TexSlot(cbuf_off, dim, false, false);
            load = fmt::format("tex{}.Load(int4(int({}), int({}), int({}), 0))", slot, R(ra), R(ra + 1), R(rb)); break;
        case 12: slot = TexSlot(cbuf_off, dim, false, false);
            load = fmt::format("tex{}.Load(int3(int({}), int({}), int({})))", slot, R(ra), R(ra + 1), R(rb)); break;
        case 14: array = true; slot = TexSlot(cbuf_off, dim, true, false);
            load = fmt::format("tex{}.Load(int4(int({}), int({}), int({}), 0))", slot, R(ra + 1), R(rb), R(ra)); break;
        default: Unimpl(op, pc, insn); return;
        }
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
        const u32 slot = TexSlot(cbuf_off, dim, array, false);
        const u32 n = ncoord + (array ? 1 : 0) + 1;
        std::vector<u32> regs;
        for (u32 i = 0; i < 4; i++)
            regs.push_back(d == RZ ? RZ : d + i);
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
        const std::string v = dc ? fmt::format("tex{}.GatherCmp(smpc{}, {}, {})", slot, slot, coords, dref)
                                 : fmt::format("tex{}.{}({}, {})", slot, g[comp], Sampler(slot), coords);
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
        const std::string v = dc ? fmt::format("tex{}.GatherCmp(smpc{}, {}, {})", slot, slot, coords, dref)
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
            Line(fmt::format("tex{}.GetDimensions({}, tmpu.x, tmpu.y, tmpu.w); tmpu.z = 1u;", slot,
                             ra == RZ ? "0u" : R(ra)));
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
                const u32 target = u32(s32(next) + BranchOffset(insn));
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
                const u32 target = u32(s32(next) + BranchOffset(insn));
                leaders.insert(target);
                work.push_back(target);
                break;
            }
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
                res_.unimplemented.insert(fmt::format("invalid_{:04x}", u32(insn >> 48)));
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
        if (op == Op::BRA || op == Op::EXIT || op == Op::SYNC || op == Op::BRK || op == Op::CONT) {
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
            case Op::SSY: stack.push_back({'S', u32(s32(next) + BranchOffset(insn))}); break;
            case Op::PBK: stack.push_back({'B', u32(s32(next) + BranchOffset(insn))}); break;
            case Op::PCNT: stack.push_back({'C', u32(s32(next) + BranchOffset(insn))}); break;
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
                const u32 target = u32(s32(next) + BranchOffset(insn));
                push_successor(target, stack);
                if (!unconditional || Bits(insn, 0, 5) != 15)
                    push_successor(next, stack);
                break;
            }
            case Op::EXIT:
                if (!unconditional || Bits(insn, 0, 5) != 15)
                    push_successor(next, stack);
                break;
            default:
                break;
            }
            if (next == b.end && op != Op::BRA && op != Op::EXIT && op != Op::SYNC && op != Op::BRK &&
                op != Op::CONT)
                push_successor(next, stack);
        }
    }
    return true;
}

void Translator::EmitJump(u32 target) {
    auto it = blocks_.find(target);
    if (it == blocks_.end()) {
        Line("return_flag = true; // jump outside program");
        Line(has_back_edge_ ? "break;" : "");
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
    case Op::NOP: case Op::DEPBAR: case Op::MEMBAR: case Op::BAR: case Op::SSY: case Op::PBK: case Op::PCNT:
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
        if (src_size == 1) // f16 source (low half)
            s = FAbsNeg(fmt::format("f16tof32({} & 0xFFFFu)", SrcB(insn, form3(op, Op::F2F_reg, Op::F2F_cbuf))),
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
        SetR(d, fmt::format("({} << {}u) + {}", av, Bits(insn, 39, 5), b));
        if (cc) SetCcInt(R(d));
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
        SetR(d, fmt::format("({} << {}u) + {}", av, shift, b));
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
        const std::string addr = fmt::format("({} + {})", R(a), Hex(u32(off)));
        if (a == RZ) {
            SetR(d, Cbuf(index, u32(off)));
            if (size == 5 && d != RZ)
                SetR(d + 1, Cbuf(index, u32(off) + 4));
            return;
        }
        SetR(d, CbufDyn(index, addr));
        if (size == 5 && d != RZ)
            SetR(d + 1, CbufDyn(index, addr + " + 4u"));
        return;
    }
    case Op::LDL: case Op::STL: {
        uses_lmem_ = true;
        const s32 off = s32(u32(Bits(insn, 20, 24)) << 8) >> 8;
        const u32 size = u32(Bits(insn, 48, 3));
        const u32 words = size == 5 ? 2 : size == 6 ? 4 : 1;
        const std::string addr = fmt::format("(({} + {}) >> 2u)", R(a), Hex(u32(off)));
        for (u32 i = 0; i < words; i++) {
            if (op == Op::LDL)
                SetR(d == RZ ? RZ : d + i, fmt::format("lmem[({} + {}u) & 1023u]", addr, i));
            else
                Line(fmt::format("lmem[({} + {}u) & 1023u] = {};", addr, i, R(d == RZ ? RZ : d + i)));
        }
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
    case Op::FSWZADD:
    case Op::VOTE:
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
        if (!is_fma && !is32)
            av = absneg(av, Bit(insn, 44), Bit(insn, 43));
        switch (op) {
        case Op::HADD2_reg: case Op::HMUL2_reg:
            bv = absneg(half_op(R(u32(Bits(insn, 20, 8))), u32(Bits(insn, 28, 2))), Bit(insn, 30), Bit(insn, 31));
            sat = Bit(insn, 32);
            break;
        case Op::HADD2_cbuf: case Op::HMUL2_cbuf:
            bv = absneg(half_op(CbufOperand(insn), 1), Bit(insn, 54), Bit(insn, 56));
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
    Line(has_back_edge_ ? "break;" : "return;");
}

void Translator::EmitPrologue(std::string& head) {
    // Shared helpers
    head += R"(
#pragma warning(disable : 3557 3550 4000 3570 4121 3078 3206)
cbuffer Driver : register(b13) {
    float4 drv_viewport;     // x: y flip (1/-1), y: depth scale, z: depth bias, w: unused
    uint4 drv_attrib[32];    // x: format register, y: unused
    uint4 drv_vbuf[16];      // x: stride, y: divisor, z: byte offset in vball (~0 = disabled), w: first element uploaded
    uint4 tex_swizzle[32];   // x: packed swizzle
    uint4 drv_misc;          // x: base instance, y: alpha test func, z: alpha ref (float bits), w: alpha test enable
};
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
        head += fmt::format("cbuffer CB{0} : register(b{0}) {{ uint4 c{0}[4096]; }};\n", i);
        head += fmt::format("uint CbufLoad{0}(uint a) {{ return c{0}[(a >> 4u) & 4095u][(a >> 2u) & 3u]; }}\n", i);
    }
    // Textures
    for (auto& t : res_.textures) {
        const char* type = "Texture2D";
        switch (t.dim) {
        case TexDim::Tex1D: type = t.array ? "Texture1DArray" : "Texture1D"; break;
        case TexDim::Tex2D: type = t.array ? "Texture2DArray" : "Texture2D"; break;
        case TexDim::Tex3D: type = "Texture3D"; break;
        case TexDim::Cube: type = t.array ? "TextureCubeArray" : "TextureCube"; break;
        }
        head += fmt::format("{}<float4> tex{} : register(t{});\n", type, t.slot, t.slot);
        head += fmt::format("SamplerState smp{} : register(s{});\n", t.slot, std::min<u32>(t.slot, 15));
        if (t.shadow)
            head += fmt::format("SamplerComparisonState smpc{} : register(s{});\n", t.slot, std::min<u32>(t.slot, 15));
    }
}

bool Translator::Run() {
    if (!DiscoverBlocks())
        return false;
    AnalyzeStacks();
    res_.uses_loop = has_back_edge_;

    // Body
    std::string body;
    out_.clear();
    indent_ = 1;
    if (has_back_edge_) {
        Line("uint blk = 0u;");
        Line("[loop] for (uint iter_ = 0u; iter_ < 65536u; iter_++) {");
        indent_++;
    } else {
        Line("uint blk = 0u;");
    }
    for (auto& [start, b] : blocks_) {
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
            if (is_flow) {
                std::string cond = guarded ? guard : "";
                const u32 cct = u32(Bits(insn, 0, 5));
                if (cct != 15) {
                    const std::string t = CcTest(cct);
                    cond = cond.empty() ? t : "(" + cond + " && " + t + ")";
                }
                u32 target = 0;
                if (op == Op::BRA)
                    target = u32(s32(NextPc(pc)) + BranchOffset(insn));
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
                Line(has_back_edge_ ? "break;" : "return_flag = true;");
        }
        indent_--;
        Line("}");
    }
    if (has_back_edge_) {
        Line("break;");
        indent_--;
        Line("}");
    }
    body = std::move(out_);

    // Assemble
    std::string src;
    EmitPrologue(src);
    std::string decls;
    decls += "static bool return_flag = false;\nstatic float4 tmp4;\nstatic uint4 tmpu;\nstatic bool tmpb;\n";
    for (u32 r : regs_used_)
        decls += fmt::format("static uint r{} = 0u;\n", r);
    for (u32 p : preds_used_)
        decls += fmt::format("static bool p{} = false;\n", p);
    if (cc_used_)
        decls += "static bool cc_z = false, cc_s = false, cc_c = false, cc_o = false;\n";
    if (uses_lmem_)
        decls += "static uint lmem[1024];\n";
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
        src += "    vertex_id = input.vid; instance_id = input.iid;\n";
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
        src += "    frag_coord = i.pos; frag_coord.w = 1.0f / i.pos.w; front_face = i.front;\n";
        for (u32 a : in_attrs_)
            src += fmt::format("    in_attr{0} = i.a{0};\n", a);
        src += "    Run();\n";
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
