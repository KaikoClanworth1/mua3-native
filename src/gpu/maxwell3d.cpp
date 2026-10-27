#include "gpu/maxwell3d.h"

#include "gpu/gpu.h"
#include "gpu/renderer.h"

namespace gpu {

using namespace reg3d;

Maxwell3D::Maxwell3D(Gpu& gpu) : gpu_(gpu) {
    // Sensible reset values (NVN programs everything it uses, but be safe).
    regs_[kRtControl] = 1;
    for (u32 i = 0; i < 8; i++)
        regs_[kColorMask + i] = 0x1111;
}

void Maxwell3D::CallMethod(u32 method, u32 arg) {
    if (method >= kMacroCallBase) {
        // Handled by Gpu (parameter batching); a direct call means a single-parameter macro.
        CallMacro((method - kMacroCallBase) >> 1, {arg});
        return;
    }
    if (method >= regs_.size())
        return;
    regs_[method] = arg;
    switch (method) {
    case kMacroUploadAddr:
        regs_[0xF00] = arg; // upload cursor
        break;
    case kMacroUploadData:
        macro_code_[regs_[0xF00]++ & 0x1FFF] = arg;
        break;
    case kMacroBind:
        macro_entry_[regs_[kMacroEntry]++ & 0x7F] = arg;
        break;
    case kI2MExec:
    case kI2MData:
        ProcessI2M(method, arg);
        break;
    case kCbData + 0: case kCbData + 1: case kCbData + 2: case kCbData + 3:
    case kCbData + 4: case kCbData + 5: case kCbData + 6: case kCbData + 7:
    case kCbData + 8: case kCbData + 9: case kCbData + 10: case kCbData + 11:
    case kCbData + 12: case kCbData + 13: case kCbData + 14: case kCbData + 15:
        ProcessCbData(arg);
        break;
    case kCbBind + 0 * 8: ProcessCbBind(0, arg); break;
    case kCbBind + 1 * 8: ProcessCbBind(1, arg); break;
    case kCbBind + 2 * 8: ProcessCbBind(2, arg); break;
    case kCbBind + 3 * 8: ProcessCbBind(3, arg); break;
    case kCbBind + 4 * 8: ProcessCbBind(4, arg); break;
    case kIndexCount:
        index_count_written_ = true;
        break;
    case kDrawBegin: {
        draw.topology = arg & 0xFFFF;
        const bool next = arg & (1u << 26), cont = arg & (1u << 27);
        if (next)
            instance_counter_++;
        else if (!cont)
            instance_counter_ = 0;
        break;
    }
    case kDrawEnd:
        ProcessDrawEnd();
        break;
    case kClearBuffers:
        ProcessClear(arg);
        break;
    case kQueryGet:
        ProcessQuery(arg);
        break;
    case kSyncInfo: // increment a syncpoint once prior work is done (NVN's fence mechanism)
        if (auto* r = gpu_.GetRenderer())
            r->FlushCommands();
        gpu_.IncrementSyncpoint(arg & 0xFFFF);
        break;
    default:
        break;
    }
}

void Maxwell3D::ProcessDrawEnd() {
    draw.indexed = index_count_written_ && regs_[kIndexCount] != 0;
    if (draw.indexed) {
        draw.first = regs_[kIndexFirst];
        draw.count = regs_[kIndexCount];
    } else {
        draw.first = regs_[kVertexBufferFirst];
        draw.count = regs_[kVertexBufferCount];
    }
    draw.instance = instance_counter_;
    index_count_written_ = false;
    if (capture) {
        capture(*this);
        return;
    }
    if (intercept && intercept(*this))
        return;
    gpu_.draws++;
    if (auto* r = gpu_.GetRenderer(); r && (draw.count || draw.indirect_args)) {
        const u64 t0 = gpu_.NowNs();
        r->Draw();
        gpu_.draw_ns += gpu_.NowNs() - t0;
    }
}

void Maxwell3D::ProcessClear(u32 arg) {
    if (capture || suppress_clears)
        return;
    if (auto* r = gpu_.GetRenderer())
        r->Clear((arg >> 6) & 0xF, arg & 0x3F);
}

void Maxwell3D::ProcessCbData(u32 arg) {
    if (capture)
        return;
    const u64 addr = Addr(kCbAddrHigh) + regs_[kCbPos];
    gpu_.WriteGpu(addr, &arg, 4);
    regs_[kCbPos] += 4;
}

void Maxwell3D::ProcessCbBind(u32 stage, u32 arg) {
    const u32 index = (arg >> 4) & 0x1F;
    if (index >= 18)
        return;
    auto& b = cbufs_[stage][index];
    b.enabled = arg & 1;
    b.address = Addr(kCbAddrHigh);
    b.size = regs_[kCbSize];
}

void Maxwell3D::ProcessQuery(u32 arg) {
    const u64 addr = Addr(kQueryAddrHigh);
    const u32 op = arg & 3;
    if (op == 1 || op == 3)
        return; // acquire / trap
    const u32 value = op == 0 ? regs_[kQuerySequence] : 0;
    if (arg & (1u << 28)) {
        gpu_.WriteGpu(addr, &value, 4);
    } else {
        const u64 v[2] = {value, gpu_.GpuTimestamp()};
        gpu_.WriteGpu(addr, v, 16);
    }
}

void Maxwell3D::ProcessI2M(u32 method, u32 arg) {
    if (method == kI2MExec) {
        i2m_cursor_ = 0;
        return;
    }
    const u64 dst = Addr(kI2MDstHigh);
    const u32 line_len = regs_[kI2MLineLength], lines = std::max<u32>(regs_[kI2MLineCount], 1);
    const u32 total = line_len * lines;
    if (i2m_cursor_ < total) {
        const u32 line = line_len ? i2m_cursor_ / line_len : 0;
        const u32 col = line_len ? i2m_cursor_ % line_len : i2m_cursor_;
        const u64 a = dst + u64(line) * regs_[kI2MPitch] + col;
        const u32 n = std::min<u32>(4, total - i2m_cursor_);
        gpu_.WriteGpu(a, &arg, n);
        if (auto* r = gpu_.GetRenderer())
            r->InvalidateRegion(a, n);
    }
    i2m_cursor_ += 4;
}

void Maxwell3D::CallMacro(u32 index, const std::vector<u32>& params) {
    if (index >= macro_entry_.size())
        return;
    ExecuteMacro(macro_entry_[index], params);
}

// ---------------------------------------------------------------- MME interpreter
void Maxwell3D::ExecuteMacro(u32 start, const std::vector<u32>& params) {
    u32 r[8]{};
    size_t next_param = 0;
    auto fetch = [&]() -> u32 { return next_param < params.size() ? params[next_param++] : 0; };
    r[1] = fetch();
    u32 method_addr = 0, method_inc = 0;
    bool carry = false;
    u32 pc = start;
    s64 delayed_pc = -1;
    auto set_reg = [&](u32 i, u32 v) {
        if (i)
            r[i] = v;
    };
    auto send = [&](u32 v) {
        CallMethod(method_addr, v);
        method_addr += method_inc;
    };
    auto set_method = [&](u32 v) {
        method_addr = v & 0xFFF;
        method_inc = (v >> 12) & 0x3F;
    };
    auto process_result = [&](u32 op, u32 dst, u32 result) {
        switch (op) {
        case 0: set_reg(dst, fetch()); break;                                     // IgnoreAndFetch
        case 1: set_reg(dst, result); break;                                      // Move
        case 2: set_reg(dst, result); set_method(result); break;                  // MoveAndSetMethod
        case 3: set_reg(dst, fetch()); send(result); break;                       // FetchAndSend
        case 4: set_reg(dst, result); send(result); break;                        // MoveAndSend
        case 5: set_reg(dst, fetch()); set_method(result); break;                 // FetchAndSetMethod
        case 6: set_reg(dst, result); set_method(result); send(fetch()); break;   // MoveAndSetMethodFetchAndSend
        case 7: set_reg(dst, result); set_method(result); send((result >> 12) & 0x3F); break;
        }
    };

    // Executes one instruction; returns false when the macro exits.
    std::function<bool(bool)> step = [&](bool is_delay_slot) -> bool {
        const u32 base = pc;
        const u32 op = macro_code_[pc & 0x1FFF];
        if (delayed_pc >= 0) {
            pc = u32(delayed_pc);
            delayed_pc = -1;
        } else {
            pc++;
        }
        const u32 operation = op & 7;
        const u32 result_op = (op >> 4) & 7;
        const bool is_exit = (op >> 7) & 1;
        const u32 dst = (op >> 8) & 7, a = (op >> 11) & 7, b = (op >> 14) & 7;
        const s32 imm = s32(op) >> 14;
        const u32 bf_src = (op >> 17) & 0x1F, bf_size = (op >> 22) & 0x1F, bf_dst = (op >> 27) & 0x1F;
        const u32 mask = bf_size >= 32 ? 0xFFFFFFFFu : ((1u << bf_size) - 1);
        switch (operation) {
        case 0: { // ALU
            const u32 alu = (op >> 17) & 0x1F;
            const u32 va = r[a], vb = r[b];
            u32 res = 0;
            switch (alu) {
            case 0: { const u64 t = u64(va) + vb; carry = t >> 32; res = u32(t); break; }
            case 1: { const u64 t = u64(va) + vb + (carry ? 1 : 0); carry = t >> 32; res = u32(t); break; }
            case 2: { const u64 t = u64(va) - vb; carry = t < 0x100000000ull; res = u32(t); break; }
            case 3: { const u64 t = u64(va) - vb - (carry ? 0 : 1); carry = t < 0x100000000ull; res = u32(t); break; }
            case 8: res = va ^ vb; break;
            case 9: res = va | vb; break;
            case 10: res = va & vb; break;
            case 11: res = va & ~vb; break;
            case 12: res = ~(va & vb); break;
            default: LOG_WARN("MME", "Unknown ALU op {}", alu);
            }
            process_result(result_op, dst, res);
            break;
        }
        case 1: // AddImmediate
            process_result(result_op, dst, r[a] + u32(imm));
            break;
        case 2: { // ExtractInsert
            u32 d = r[a];
            const u32 s = (r[b] >> bf_src) & mask;
            d &= ~(mask << bf_dst);
            d |= s << bf_dst;
            process_result(result_op, dst, d);
            break;
        }
        case 3: { // ExtractShiftLeftImmediate
            const u32 res = ((r[a] >> r[b]) & mask) << bf_dst;
            process_result(result_op, dst, res);
            break;
        }
        case 4: { // ExtractShiftLeftRegister
            const u32 res = ((r[a] >> bf_src) & mask) << r[b];
            process_result(result_op, dst, res);
            break;
        }
        case 5: // Read
            process_result(result_op, dst, regs_[(r[a] + u32(imm)) & 0xFFF]);
            break;
        case 7: { // Branch
            const bool not_zero = (op >> 4) & 1;
            const bool annul = (op >> 5) & 1;
            const bool taken = not_zero ? r[a] != 0 : r[a] == 0;
            if (taken) {
                if (annul) {
                    pc = base + u32(imm);
                    return true;
                }
                delayed_pc = s64(base + u32(imm));
                return step(true);
            }
            break;
        }
        default:
            LOG_WARN("MME", "Unknown MME operation {}", operation);
        }
        if (is_exit && !is_delay_slot) {
            step(true); // exit has a delay slot
            return false;
        }
        return true;
    };

    for (int guard = 0; guard < 0x100000 && step(false); guard++) {
    }
}

} // namespace gpu
