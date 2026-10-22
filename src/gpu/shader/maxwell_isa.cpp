#include "gpu/shader/maxwell_isa.h"

#include <algorithm>
#include <vector>

namespace gpu::shader {

namespace {

struct Pattern {
    Op op;
    u16 mask, expect;
    int fixed_bits;
};

std::vector<Pattern> BuildTable() {
    std::vector<Pattern> t;
    auto add = [&](Op op, const char* pat) {
        u16 mask = 0, expect = 0;
        int fixed = 0;
        for (int i = 0; i < 16; i++) {
            const char c = pat[i];
            const u16 bit = u16(1u << (15 - i));
            if (c == '0' || c == '1') {
                mask |= bit;
                fixed++;
                if (c == '1')
                    expect |= bit;
            }
        }
        t.push_back({op, mask, expect, fixed});
    };
#define X(name, pat) add(Op::name, pat);
    MAXWELL_OPCODES(X)
#undef X
    // Most specific patterns first.
    std::stable_sort(t.begin(), t.end(), [](const Pattern& a, const Pattern& b) { return a.fixed_bits > b.fixed_bits; });
    return t;
}

const std::vector<Pattern>& Table() {
    static const std::vector<Pattern> t = BuildTable();
    return t;
}

} // namespace

Op Decode(u64 insn) {
    const u16 top = u16(insn >> 48);
    for (const auto& p : Table())
        if ((top & p.mask) == p.expect)
            return p.op;
    return Op::Invalid;
}

const char* OpName(Op op) {
    static const char* names[] = {
#define X(name, pat) #name,
        MAXWELL_OPCODES(X)
#undef X
        "Invalid"};
    return names[size_t(op)];
}

} // namespace gpu::shader
