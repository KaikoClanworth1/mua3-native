#include <cstdlib>
#include <atomic>
#include <sstream>
#include "core/system.h"

#include <algorithm>
#include <map>

#include "core/game_patches.h"
#include "core/hle/camera_shake.h"
#include "core/hle/menu_mods.h"
#include "core/hle/native_trace.h"
#include "core/hle/service.h"
#include "core/kernel/kernel.h"
#include "core/memory.h"
#include "gpu/gpu.h"
#include "loader/formats.h"

namespace core {

static System* g_system = nullptr;
System& Sys() { return *g_system; }

namespace {
struct Symbol {
    u64 addr, size;
    std::string name;
};
std::map<u64, Symbol> g_symbols;
std::map<std::string, u64> g_symbol_by_name;

void LoadSymbols(Memory& m, const LoadedModule& mod) {
    const u32 mod0_off = m.Read<u32>(mod.base + 4);
    const u64 mod0 = mod.base + mod0_off;
    if (m.Read<u32>(mod0) != 0x30444F4D) // "MOD0"
        return;
    const u64 dyn = mod0 + s32(m.Read<u32>(mod0 + 4));
    u64 symtab = 0, strtab = 0, strsz = 0;
    for (u64 p = dyn; p < mod.base + mod.size; p += 16) {
        const s64 tag = m.Read<s64>(p);
        const u64 val = m.Read<u64>(p + 8);
        if (tag == 0)
            break;
        if (tag == 6)
            symtab = mod.base + val;
        else if (tag == 5)
            strtab = mod.base + val;
        else if (tag == 10)
            strsz = val;
    }
    if (!symtab || !strtab || strtab <= symtab)
        return;
    const u64 count = (strtab - symtab) / 24;
    size_t n = 0;
    for (u64 i = 0; i < count; i++) {
        const u64 e = symtab + i * 24;
        const u32 name = m.Read<u32>(e);
        const u8 info = m.Read<u8>(e + 4);
        const u16 shndx = m.Read<u16>(e + 6);
        const u64 value = m.Read<u64>(e + 8);
        const u64 size = m.Read<u64>(e + 16);
        if (!shndx || !value || (info & 0xF) != 2 /*FUNC*/ || name >= strsz)
            continue;
        Symbol s{mod.base + value, size, m.ReadString(strtab + name, 512)};
        g_symbol_by_name[s.name] = s.addr;
        g_symbols[s.addr] = std::move(s);
        n++;
    }
    LOG_INFO("Loader", "{}: {} function symbols", mod.name, n);
}
} // namespace

u64 FindSymbol(const std::string& name) {
    auto it = g_symbol_by_name.find(name);
    return it == g_symbol_by_name.end() ? 0 : it->second;
}

void System::UpdateConfig(const std::function<void(Config&, bool running)>& fn) {
    std::lock_guard lk(config_mutex_);
    fn(cfg_, true);
    config_generation_.fetch_add(1, std::memory_order_acq_rel);
    Config disk;
    disk.Load("mua3.ini");
    fn(disk, false);
    if (!disk.Save("mua3.ini"))
        LOG_WARN("System", "could not save mua3.ini");
}

System::System(const Config& cfg) : cfg_(cfg) {
    g_system = this;
    memory_ = std::make_unique<Memory>();
    gpu::Init(*memory_);
    kernel_ = std::make_unique<kernel::Kernel>(*memory_);
}

System::~System() = default;

const LoadedModule* System::ModuleFor(u64 addr) const {
    for (auto& m : modules_)
        if (addr >= m.base && addr < m.base + m.size)
            return &m;
    return nullptr;
}

std::string System::Symbolize(u64 addr) const {
    const LoadedModule* mod = ModuleFor(addr);
    if (!mod)
        return fmt::format("{:#x}", addr);
    auto it = g_symbols.upper_bound(addr);
    if (it != g_symbols.begin()) {
        --it;
        if (addr < it->second.addr + std::max<u64>(it->second.size, 4) && it->second.addr >= mod->base)
            return fmt::format("{}+{:#x} ({}+{:#x})", it->second.name, addr - it->second.addr, mod->name,
                               addr - mod->base);
    }
    return fmt::format("{}+{:#x}", mod->name, addr - mod->base);
}

bool System::Load(std::string& error) {
    const fs::path exefs = cfg_.game_dir / "exefs";
    auto npdm_data = common::ReadFile(exefs / "main.npdm");
    if (auto npdm = loader::ParseNpdm(npdm_data)) {
        main_stack_size_ = npdm->main_stack_size;
        main_prio_ = npdm->main_priority;
        main_core_ = npdm->main_core;
        LOG_INFO("Loader", "NPDM '{}' program {:016x} as={} prio={} stack={:#x}", npdm->name, npdm->program_id,
                 npdm->address_space, npdm->main_priority, npdm->main_stack_size);
        if (npdm->program_id)
            kernel_->program_id = npdm->program_id;
    }

    u64 addr = layout::kCodeBase;
    const char* order[] = {"rtld", "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4",
                           "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk"};
    for (const char* name : order) {
        auto file = common::ReadFile(exefs / name);
        if (file.empty())
            continue;
        auto img = loader::LoadNso(file);
        if (!img) {
            error = fmt::format("Failed to load NSO '{}'", name);
            return false;
        }
        const u64 size = img->image.size();
        if (!memory_->Map(addr, size, MS_CodeData, MP_RW)) {
            error = "Failed to map module memory";
            return false;
        }
        memory_->WriteBlock(addr, img->image.data(), size);
        ApplyGamePatches(*memory_, name, addr, img->module_id, cfg_);
        memory_->SetState(addr + img->text_off, common::AlignUp(img->text_size, 0x1000), MS_Code, MP_RX);
        memory_->SetState(addr + img->ro_off, common::AlignUp(img->ro_size, 0x1000), MS_Code, MP_R);
        memory_->SetState(addr + img->data_off, size - img->data_off, MS_CodeData, MP_RW);
        LoadedModule mod;
        mod.name = name;
        mod.base = addr;
        mod.size = size;
        mod.text = addr + img->text_off;
        mod.text_size = img->text_size;
        mod.ro = addr + img->ro_off;
        mod.ro_size = img->ro_size;
        mod.data = addr + img->data_off;
        mod.data_size = img->data_size;
        LOG_INFO("Loader", "{:8} at {:#012x}-{:#012x}", name, addr, addr + size);
        modules_.push_back(mod);
        addr += size; // rtld discovers modules by walking QueryMemory, so keep them contiguous
    }
    if (modules_.empty()) {
        error = "No executable modules found in " + exefs.string();
        return false;
    }
    for (auto& mod : modules_)
        LoadSymbols(*memory_, mod);
    InstallTraces();
    for (auto& mod : modules_)
        if (mod.name == "main") {
            hle::menu::Install(*memory_, *kernel_, mod.base);
            hle::camera::Install(*memory_, *kernel_, mod.base, cfg_);
        }
    hle::trace::Install(*kernel_, modules_);

    // Main thread stack
    const u64 stack_size = common::AlignUp(main_stack_size_, 0x1000);
    const u64 stack = memory_->FindFree(layout::kKernelMapBase, 0x1000000000ull, stack_size + 0x10000);
    memory_->Map(stack, stack_size, MS_Stack, MP_RW);
    main_stack_top_ = stack + stack_size;
    return true;
}

// MUA3_TRACE_FN=main+0x10f3ac0,sdk+0x85670,...: log each entry to these guest functions (debugging aid).
void System::InstallTraces() {
    const char* env = std::getenv("MUA3_TRACE_FN");
    if (!env)
        return;
    trace_armed_ = std::getenv("MUA3_TRACE_DEFER") == nullptr; // deferred: wait for the "trace" command
    std::stringstream ss(env);
    for (std::string item; std::getline(ss, item, ',');) {
        const size_t plus = item.find('+');
        if (plus == std::string::npos)
            continue;
        const std::string modname = item.substr(0, plus);
        const u64 off = std::strtoull(item.c_str() + plus + 1, nullptr, 16);
        for (auto& m : modules_) {
            if (m.name != modname)
                continue;
            auto count = std::make_shared<std::atomic<u64>>(0);
            trace_counters_.push_back(count);
            kernel_->InstallTrace(m.base + off, item, [this, item, count](kernel::Thread* t) {
                if (!trace_armed_)
                    return;
                const u64 n = (*count)++;
                if (n >= 300 && n % 10000)
                    return;
                auto& c = *t->cpu;
                LOG_INFO("Trace", "{} #{} t{} x0={:#x} x1={:#x} x2={:#x} x3={:#x} lr={}", item, n, t->id, c.Reg(0),
                         c.Reg(1), c.Reg(2), c.Reg(3), Symbolize(c.Reg(30)));
            });
            LOG_INFO("Trace", "tracing {}", item);
        }
    }
}

void System::ArmTraces() {
    for (auto& c : trace_counters_)
        *c = 0;
    trace_armed_ = true;
    LOG_INFO("Trace", "tracing armed");
}

void System::Start() {
    auto t = kernel_->CreateThread(modules_[0].base, 0, main_stack_top_, main_prio_, main_core_, "main");
    t->cpu->SetReg(0, 0);
    t->cpu->SetReg(1, t->handle);
    kernel_->StartThread(t);
}

} // namespace core
