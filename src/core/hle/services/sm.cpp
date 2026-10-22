#include "core/hle/service.h"

namespace hle {

namespace {

// Fallback for services that aren't implemented: every command succeeds with no output.
class StubService final : public Service {
public:
    explicit StubService(std::string n) : Service(std::move(n)) {}
    void Dispatch(Request& r) override {
        LOG_WARN("Stub", "{}: command {} (stubbed)", name_, r.cmd);
    }
};

class SmService final : public Service {
public:
    explicit SmService(ServiceManager& sm) : Service("sm:"), sm_(sm) {
        Ok(0, "Initialize");
        Reg(1, "GetService", [this](Request& r) {
            const u64 raw = r.Pop<u64>();
            char name[9]{};
            std::memcpy(name, &raw, 8);
            ServicePtr s = sm_.GetService(name);
            if (!s) {
                LOG_WARN("sm", "GetService('{}'): not implemented, using stub", name);
                s = std::make_shared<StubService>(name);
            } else {
                LOG_INFO("sm", "GetService('{}')", name);
            }
            r.PushInterface(s);
        });
        Ok(2, "RegisterService");
        Ok(3, "UnregisterService");
    }

private:
    ServiceManager& sm_;
};

} // namespace

ServicePtr MakeStub(std::string name) { return std::make_shared<StubService>(std::move(name)); }

void RegisterSm(ServiceManager& sm) {
    sm.RegisterService("sm:", [&sm] { return std::make_shared<SmService>(sm); });
}

} // namespace hle
