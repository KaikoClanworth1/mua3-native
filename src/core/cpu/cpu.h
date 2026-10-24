#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include "common/common.h"

namespace core {
class Memory;
}

namespace cpu {

using Vec128 = std::array<u64, 2>;

struct CpuCallbacks {
    std::function<void(u32 imm)> svc;                  // SVC #imm
    std::function<void(u64 pc, int kind)> exception;   // BRK / UDF / etc.
    std::function<u64()> cntpct;                       // 19.2 MHz counter
};

// Abstract guest CPU. Today it is backed by dynarmic; recompiled functions call back through the same register
// file so that guest and native code can be mixed freely.
class Core {
public:
    virtual ~Core() = default;
    virtual void Run() = 0;  // returns once Halt() has been requested
    virtual void Halt() = 0; // safe to call from SVC handlers on this thread
    virtual void Step() = 0;

    virtual u64 Reg(int i) const = 0;
    virtual void SetReg(int i, u64 v) = 0;
    virtual u64 PC() const = 0;
    virtual void SetPC(u64 v) = 0;
    virtual u64 SP() const = 0;
    virtual void SetSP(u64 v) = 0;
    virtual Vec128 Vector(int i) const = 0;
    virtual void SetVector(int i, Vec128 v) = 0;
    virtual u32 Fpcr() const = 0;
    virtual void SetFpcr(u32 v) = 0;
    virtual u32 Fpsr() const = 0;
    virtual void SetFpsr(u32 v) = 0;
    virtual u32 Pstate() const = 0;
    virtual void SetPstate(u32 v) = 0;
    virtual u64 TpidrEl0() const = 0;
    virtual void SetTpidrEl0(u64 v) = 0;
    virtual void SetTpidrroEl0(u64 v) = 0;
    virtual void InvalidateCache(u64 addr, u64 size) = 0;
    // Runs `fn` on this core's own thread at the next block boundary.
    virtual void RunOnCore(std::function<void()> fn) = 0;
    virtual void ClearExclusive() = 0;
    virtual std::string Disassemble(u64 addr, int count) const { return {}; }
};

// Created once per process; owns the exclusive monitor shared by all cores.
class CoreFactory {
public:
    virtual ~CoreFactory() = default;
    virtual std::unique_ptr<Core> Create(CpuCallbacks cb, size_t slot) = 0;
    // Invalidate translated code in every live core (after code patching / module load).
    virtual void InvalidateAll(u64 addr, u64 size) = 0;
};

std::unique_ptr<CoreFactory> CreateDynarmicFactory(core::Memory& memory, size_t max_cores);

// Share of guest instructions run by recompiled code over the last 10 s (percent; < 0 until measured).
extern std::atomic<float> g_native_percent;

} // namespace cpu
