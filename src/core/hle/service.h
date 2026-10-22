#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>

#include "core/kernel/kernel.h"

namespace hle {

using kernel::Handle;
using kernel::Result;

class Service;
using ServicePtr = std::shared_ptr<Service>;

struct IpcBuffer {
    u64 addr = 0;
    u64 size = 0;
};

// One parsed request plus the response being built.
class Request {
public:
    kernel::Kernel& k;
    kernel::Thread* thread;
    core::Memory& mem;

    u32 type = 0;
    u32 cmd = 0;
    bool is_domain = false;
    u32 domain_cmd = 0, object_id = 0;
    std::vector<u32> in_objects;
    std::vector<IpcBuffer> x, a, b, w, c;
    std::vector<Handle> copy_handles, move_handles;
    u64 pid = 0;

    // Raw arguments (after the SFCI header)
    std::vector<u8> args;
    size_t arg_pos = 0;

    template <typename T>
    T Pop() {
        arg_pos = common::AlignUp(arg_pos, alignof(T) < 4 ? alignof(T) : (sizeof(T) >= 8 ? 8 : alignof(T)));
        T v{};
        if (arg_pos + sizeof(T) <= args.size())
            std::memcpy(&v, args.data() + arg_pos, sizeof(T));
        arg_pos += sizeof(T);
        return v;
    }
    template <typename T>
    T ArgAt(size_t off) const {
        T v{};
        if (off + sizeof(T) <= args.size())
            std::memcpy(&v, args.data() + off, sizeof(T));
        return v;
    }

    // ---- response
    Result result = 0;
    std::vector<u8> out;
    std::vector<Handle> out_copy, out_move;
    std::vector<ServicePtr> out_objects;

    template <typename T>
    void Push(const T& v) {
        const size_t al = sizeof(T) >= 8 ? 8 : (sizeof(T) >= 4 ? 4 : sizeof(T));
        out.resize(common::AlignUp(out.size(), al));
        const size_t p = out.size();
        out.resize(p + sizeof(T));
        std::memcpy(out.data() + p, &v, sizeof(T));
    }
    void PushRaw(const void* d, size_t n) {
        const size_t p = out.size();
        out.resize(p + n);
        std::memcpy(out.data() + p, d, n);
    }
    void PushCopy(Handle h) { out_copy.push_back(h); }
    void PushMove(Handle h) { out_move.push_back(h); }
    void PushCopy(kernel::KObjectPtr obj);
    void PushInterface(ServicePtr s) { out_objects.push_back(std::move(s)); }
    void SetError(Result r) { result = r; }

    // Buffers. "In" picks A or X, "Out" picks B or C (auto-select semantics).
    IpcBuffer InBuffer(size_t i = 0) const;
    IpcBuffer OutBuffer(size_t i = 0) const;
    std::vector<u8> ReadIn(size_t i = 0) const;
    std::string ReadInString(size_t i = 0) const;
    size_t WriteOut(const void* data, size_t size, size_t i = 0);
    size_t WriteOut(const std::vector<u8>& d, size_t i = 0) { return WriteOut(d.data(), d.size(), i); }
    size_t OutSize(size_t i = 0) const { return OutBuffer(i).size; }
};

class Service : public std::enable_shared_from_this<Service> {
public:
    explicit Service(std::string name) : name_(std::move(name)) {}
    virtual ~Service() = default;
    const std::string& Name() const { return name_; }
    virtual void Dispatch(Request& r);
    virtual void OnClose() {}
    u32 PointerBufferSize() const { return pointer_buffer_size_; }

protected:
    using Handler = std::function<void(Request&)>;
    void Reg(u32 id, const char* name, Handler h) { handlers_[id] = {name, std::move(h)}; }
    // Registers a command that just succeeds with no output.
    void Ok(u32 id, const char* name) {
        handlers_[id] = {name, [](Request&) {}};
    }
    std::string name_;
    std::map<u32, std::pair<const char*, Handler>> handlers_;
    u32 pointer_buffer_size_ = 0x8000;
    bool quiet_ = false;
};

struct Domain {
    std::map<u32, ServicePtr> objects;
    u32 next_id = 1;
    u32 Add(ServicePtr s) {
        const u32 id = next_id++;
        objects[id] = std::move(s);
        return id;
    }
};

class SessionObject final : public kernel::KObject {
public:
    explicit SessionObject(ServicePtr s) : service(std::move(s)) { name = service->Name(); }
    kernel::ObjType Type() const override { return kernel::ObjType::Session; }
    void OnClose() {}
    ServicePtr service;
    std::shared_ptr<Domain> domain; // non-null once converted
};

using ServiceFactory = std::function<ServicePtr()>;

class ServiceManager {
public:
    explicit ServiceManager(kernel::Kernel& k);
    ~ServiceManager();
    std::shared_ptr<SessionObject> ConnectToPort(const std::string& name);
    Result HandleSyncRequest(kernel::Thread* t, Handle h, u64 msg, u64 size);

    void RegisterService(const std::string& name, ServiceFactory f) { factories_[name] = std::move(f); }
    ServicePtr GetService(const std::string& name);
    Handle NewSessionHandle(ServicePtr s);

    kernel::Kernel& k;

private:
    std::map<std::string, ServiceFactory> factories_;
    std::mutex mutex_;
};

// Registers every HLE service (services/*.cpp).
void RegisterAllServices(ServiceManager& sm);

ServicePtr MakeStub(std::string name);

inline std::shared_ptr<kernel::Event> MakeEvent(std::string name, bool signaled = false) {
    auto e = std::make_shared<kernel::Event>(std::move(name));
    e->signaled = signaled;
    return e;
}

// Small helper for services that hand out sub-interfaces.
template <typename T, typename... A>
inline ServicePtr Make(A&&... a) {
    return std::make_shared<T>(std::forward<A>(a)...);
}

using u128 = std::array<u64, 2>;

// Common result codes
namespace err {
constexpr Result FsPathNotFound = 0x202;         // fs 1
constexpr Result FsPathAlreadyExists = 0x402;    // fs 2
constexpr Result SmNotRegistered = 0xE15;        // sm 7
constexpr Result Unimplemented = 0x10A01;
} // namespace err

} // namespace hle
