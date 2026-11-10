// MUA3_TRACE_NATIVE=main+0x1cf9f0,main+0x6e0890,...: logs calls to recompiled guest functions (x0-x3 and the
// caller), once per distinct (caller, x1) pair, up to 2,000 lines per function. Works with native code, unlike
// MUA3_TRACE_FN (which patches guest code and so only ever fired under the JIT). Debugging aid.
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

#include "core/cpu/cpu.h"
#include "core/cpu/recomp/recomp_rt.h"
#include "core/kernel/kernel.h"
#include "core/system.h"

namespace hle::trace {
namespace {

struct Traced {
    std::string name;
    cpu::FunctionOverride o;
    std::set<std::pair<u64, u64>> seen;
};
std::mutex g_mutex;
std::unordered_map<u64, Traced*> g_traced;

void Hook(recomp::State& s) {
    Traced* t;
    {
        std::lock_guard lk(g_mutex);
        t = g_traced.at(s.pc);
        if (t->seen.size() < 2000 && t->seen.emplace(s.x[30], s.x[1]).second)
            LOG_INFO("NTrace", "{} x0={:#x} x1={:#x} x2={:#x} x3={:#x} from {}", t->name, s.x[0], s.x[1], s.x[2], s.x[3],
                     core::Sys().Symbolize(s.x[30]));
    }
    *t->o.ok = 1;
    t->o.original.load()(s);
    *t->o.ok = 0;
}

} // namespace

void Install(kernel::Kernel& kernel, const std::vector<core::LoadedModule>& modules) {
    const char* env = std::getenv("MUA3_TRACE_NATIVE");
    if (!env)
        return;
    std::stringstream ss(env);
    for (std::string item; std::getline(ss, item, ',');) {
        const size_t plus = item.find('+');
        if (plus == std::string::npos)
            continue;
        for (auto& m : modules)
            if (m.name == item.substr(0, plus)) {
                auto* t = new Traced;
                t->name = item;
                t->o.pc = m.base + std::strtoull(item.c_str() + plus + 1, nullptr, 16);
                t->o.host = Hook;
                t->o.direct = true;
                g_traced[t->o.pc] = t;
                kernel.Cpus().Override(t->o);
                LOG_INFO("NTrace", "tracing {}", item);
            }
    }
}

} // namespace hle::trace
