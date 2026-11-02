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
    if (method >= regs_.size())
        return;
    regs_[method] = arg;
    switch (method) {
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
    gpu_.draws++;
    if (auto* r = gpu_.GetRenderer(); r && (draw.count || draw.indirect_args)) {
        const u64 t0 = gpu_.NowNs();
        r->Draw();
        gpu_.draw_ns += gpu_.NowNs() - t0;
    }
}

void Maxwell3D::ProcessClear(u32 arg) {
    if (capture)
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


} // namespace gpu
