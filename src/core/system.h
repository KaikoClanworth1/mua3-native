#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
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
    void InstallTraces();
    void ArmTraces();

    Memory& Mem() { return *memory_; }
    kernel::Kernel& Kern() { return *kernel_; }
    const Config& Cfg() const { return cfg_; }
    // Settings changed while the game runs (the in-game Graphics menu): `fn(config, running)` edits the running
    // config (running = true: only what can change while the game runs) and mua3.ini (running = false; other
    // settings in the file, and command-line overrides, are left alone). Readers that apply settings outside the
    // game (the window, the overlay) watch ConfigGeneration().
    void UpdateConfig(const std::function<void(Config&, bool running)>& fn);
    u32 ConfigGeneration() const { return config_generation_.load(std::memory_order_acquire); }
    Config CfgSnapshot() const {
        std::lock_guard lk(config_mutex_);
        return cfg_;
    }
    // The game asked to close (e.g. the pause menu's Quit Game): the main loop shuts down as for a closed window.
    void RequestExit() { exit_requested_.store(true, std::memory_order_release); }
    bool ExitRequested() const { return exit_requested_.load(std::memory_order_acquire); }

private:
    Config cfg_;
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<kernel::Kernel> kernel_;
    std::vector<LoadedModule> modules_;
    std::vector<std::shared_ptr<std::atomic<u64>>> trace_counters_;
    std::atomic<bool> trace_armed_{true};
    std::atomic<bool> exit_requested_{false};
    mutable std::mutex config_mutex_;
    std::atomic<u32> config_generation_{0};
    u64 main_stack_top_ = 0;
    u32 main_stack_size_ = 0x100000;
    s32 main_prio_ = 44;
    s32 main_core_ = 0;
};

System& Sys();

} // namespace core
