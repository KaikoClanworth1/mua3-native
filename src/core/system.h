#pragma once
#include <memory>
#include "common/common.h"
#include "core/config.h"

namespace core {
class Memory;
}
namespace kernel {
class Kernel;
}

namespace core {

struct LoadedModule {
    std::string name;
    u64 base = 0, size = 0;
    u64 text = 0, text_size = 0, ro = 0, ro_size = 0, data = 0, data_size = 0;
};

class System {
public:
    explicit System(const Config& cfg);
    ~System();

    bool Load(std::string& error);
    void Start(); // starts the main guest thread
    const std::vector<LoadedModule>& Modules() const { return modules_; }
    const LoadedModule* ModuleFor(u64 addr) const;
    std::string Symbolize(u64 addr) const;

    Memory& Mem() { return *memory_; }
    kernel::Kernel& Kern() { return *kernel_; }
    const Config& Cfg() const { return cfg_; }

private:
    Config cfg_;
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<kernel::Kernel> kernel_;
    std::vector<LoadedModule> modules_;
    u64 main_stack_top_ = 0;
    u32 main_stack_size_ = 0x100000;
    s32 main_prio_ = 44;
    s32 main_core_ = 0;
};

System& Sys();

} // namespace core
