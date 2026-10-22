#include <algorithm>

#include "core/hle/service.h"

namespace hle {

using common::AlignUp;

// ---------------------------------------------------------------- Request helpers
void Request::PushCopy(kernel::KObjectPtr obj) { out_copy.push_back(k.Handles().Add(std::move(obj))); }

IpcBuffer Request::InBuffer(size_t i) const {
    if (i < a.size() && a[i].addr && a[i].size)
        return a[i];
    if (i < x.size() && x[i].addr)
        return x[i];
    if (i < a.size())
        return a[i];
    return {};
}

IpcBuffer Request::OutBuffer(size_t i) const {
    if (i < b.size() && b[i].addr && b[i].size)
        return b[i];
    if (i < c.size() && c[i].addr)
        return c[i];
    if (i < b.size())
        return b[i];
    return {};
}

std::vector<u8> Request::ReadIn(size_t i) const {
    const IpcBuffer buf = InBuffer(i);
    std::vector<u8> v(size_t(buf.size));
    if (buf.size)
        mem.ReadBlock(buf.addr, v.data(), v.size());
    return v;
}

std::string Request::ReadInString(size_t i) const {
    auto v = ReadIn(i);
    return std::string(reinterpret_cast<const char*>(v.data()), strnlen(reinterpret_cast<const char*>(v.data()), v.size()));
}

size_t Request::WriteOut(const void* data, size_t size, size_t i) {
    const IpcBuffer buf = OutBuffer(i);
    const size_t n = std::min<size_t>(size, size_t(buf.size));
    if (n)
        mem.WriteBlock(buf.addr, data, n);
    return n;
}

// ---------------------------------------------------------------- Service
void Service::Dispatch(Request& r) {
    auto it = handlers_.find(r.cmd);
    if (it == handlers_.end()) {
        LOG_WARN("Service", "{}: unimplemented command {} (returning success)", name_, r.cmd);
        return;
    }
    if (!quiet_)
        LOG_DEBUG("Service", "{}::{} ({})", name_, it->second.first, r.cmd);
    it->second.second(r);
}

// ---------------------------------------------------------------- ServiceManager
ServiceManager::ServiceManager(kernel::Kernel& kk) : k(kk) { RegisterAllServices(*this); }
ServiceManager::~ServiceManager() = default;

ServicePtr ServiceManager::GetService(const std::string& name) {
    std::lock_guard lk(mutex_);
    auto it = factories_.find(name);
    if (it == factories_.end())
        return nullptr;
    return it->second();
}

std::shared_ptr<SessionObject> ServiceManager::ConnectToPort(const std::string& name) {
    auto s = GetService(name);
    return s ? std::make_shared<SessionObject>(s) : nullptr;
}

Handle ServiceManager::NewSessionHandle(ServicePtr s) {
    return k.Handles().Add(std::make_shared<SessionObject>(std::move(s)));
}

namespace {

constexpr u32 kSFCI = 0x49434653;
constexpr u32 kSFCO = 0x4F434653;

struct Parsed {
    u32 data_words_off; // word index of raw data start (unaligned)
    u32 data_words;
};

} // namespace

Result ServiceManager::HandleSyncRequest(kernel::Thread* t, Handle h, u64 msg, u64 msg_size) {
    auto session = k.Handles().GetAs<SessionObject>(h);
    if (!session) {
        LOG_ERROR("IPC", "SendSyncRequest on invalid handle {:#x}", h);
        return kernel::rc::InvalidHandle;
    }
    core::Memory& m = k.Mem();
    const u32* wp = m.Ptr<u32>(msg);
    Request r{k, t, m};

    const u32 w0 = wp[0], w1 = wp[1];
    r.type = w0 & 0xFFFF;
    const u32 nx = (w0 >> 16) & 0xF, na = (w0 >> 20) & 0xF, nb = (w0 >> 24) & 0xF, nw = (w0 >> 28) & 0xF;
    const u32 data_words = w1 & 0x3FF;
    const u32 cflags = (w1 >> 10) & 0xF;
    const bool has_hd = w1 >> 31;
    u32 pos = 2;
    if (has_hd) {
        const u32 hd = wp[pos++];
        const bool send_pid = hd & 1;
        const u32 ncopy = (hd >> 1) & 0xF, nmove = (hd >> 5) & 0xF;
        if (send_pid) {
            r.pid = wp[pos] | (u64(wp[pos + 1]) << 32);
            pos += 2;
        }
        for (u32 i = 0; i < ncopy; i++)
            r.copy_handles.push_back(wp[pos++]);
        for (u32 i = 0; i < nmove; i++)
            r.move_handles.push_back(wp[pos++]);
    }
    for (u32 i = 0; i < nx; i++) {
        const u32 a = wp[pos], b = wp[pos + 1];
        pos += 2;
        IpcBuffer buf;
        buf.addr = b | (u64((a >> 12) & 0xF) << 32) | (u64((a >> 6) & 0x7) << 36);
        buf.size = a >> 16;
        r.x.push_back(buf);
    }
    auto parse_abw = [&](u32 n, std::vector<IpcBuffer>& out) {
        for (u32 i = 0; i < n; i++) {
            const u32 s = wp[pos], a = wp[pos + 1], f = wp[pos + 2];
            pos += 3;
            IpcBuffer buf;
            buf.addr = a | (u64((f >> 28) & 0xF) << 32) | (u64((f >> 2) & 0x7) << 36);
            buf.size = s | (u64((f >> 24) & 0xF) << 32);
            out.push_back(buf);
        }
    };
    parse_abw(na, r.a);
    parse_abw(nb, r.b);
    parse_abw(nw, r.w);
    const u32 raw_start_word = pos;
    const u64 raw_addr = AlignUp(msg + raw_start_word * 4, 16);
    // C descriptors follow the raw data.
    if (cflags >= 2) {
        const u32 nc = cflags == 2 ? 1 : cflags - 2;
        u32 cp = raw_start_word + data_words;
        for (u32 i = 0; i < nc; i++) {
            const u32 a = wp[cp], b = wp[cp + 1];
            cp += 2;
            r.c.push_back({a | (u64(b & 0xFFFF) << 32), b >> 16});
        }
    }

    const u64 raw_end = msg + u64(raw_start_word + data_words) * 4;
    auto respond = [&](bool domain_reply) {
        // Turn returned interfaces into handles / domain object ids.
        std::vector<u32> out_ids;
        for (auto& s : r.out_objects) {
            if (domain_reply)
                out_ids.push_back(session->domain->Add(s));
            else
                r.out_move.push_back(NewSessionHandle(s));
        }
        std::vector<u32> words(2);
        const bool hd = !r.out_copy.empty() || !r.out_move.empty();
        if (hd) {
            words.push_back(u32(r.out_copy.size() << 1 | r.out_move.size() << 5));
            for (Handle hh : r.out_copy)
                words.push_back(hh);
            for (Handle hh : r.out_move)
                words.push_back(hh);
        }
        const u32 raw_word = u32(words.size());
        const u64 raw_out = AlignUp(msg + raw_word * 4, 16);
        const u32 pad = u32(raw_out - (msg + raw_word * 4));
        std::vector<u8> raw;
        if (domain_reply) {
            u32 dh[4] = {u32(out_ids.size()), 0, 0, 0};
            raw.insert(raw.end(), (u8*)dh, (u8*)dh + 16);
        }
        u32 hdr[4] = {kSFCO, 0, r.result, 0};
        raw.insert(raw.end(), (u8*)hdr, (u8*)hdr + 16);
        raw.insert(raw.end(), r.out.begin(), r.out.end());
        raw.resize(AlignUp(raw.size(), 4));
        for (u32 id : out_ids)
            raw.insert(raw.end(), (u8*)&id, (u8*)&id + 4);
        const u32 total_words = u32((raw.size() + 16) / 4); // include alignment slack like the official SDK
        words[0] = 0;
        words[1] = total_words | (hd ? 0x80000000u : 0);
        u8* base = m.Ptr(msg);
        const size_t limit = size_t(msg_size);
        if (words.size() * 4 + pad + raw.size() > limit)
            PANIC("IPC response too large ({} bytes) for {}", words.size() * 4 + pad + raw.size(), session->name);
        std::memcpy(base, words.data(), words.size() * 4);
        std::memset(base + words.size() * 4, 0, pad);
        std::memcpy(base + words.size() * 4 + pad, raw.data(), raw.size());
    };

    switch (r.type) {
    case 2: // Close
        return 0;
    case 5:
    case 7: { // Control
        const u32* hdr = m.Ptr<u32>(raw_addr);
        r.cmd = hdr[2];
        switch (r.cmd) {
        case 0: // ConvertCurrentObjectToDomain
            if (!session->domain) {
                session->domain = std::make_shared<Domain>();
                const u32 id = session->domain->Add(session->service);
                r.Push<u32>(id);
            } else {
                r.Push<u32>(1);
            }
            break;
        case 1: { // CopyFromCurrentDomain
            const u32 id = hdr[4];
            auto it = session->domain ? session->domain->objects.find(id) : decltype(session->domain->objects.end()){};
            if (!session->domain || it == session->domain->objects.end()) {
                r.result = 0x1A01;
                break;
            }
            r.out_move.push_back(NewSessionHandle(it->second));
            break;
        }
        case 2:   // CloneCurrentObject
        case 4: { // CloneCurrentObjectEx
            auto clone = std::make_shared<SessionObject>(session->service);
            clone->domain = session->domain;
            r.out_move.push_back(k.Handles().Add(clone));
            break;
        }
        case 3: // QueryPointerBufferSize
            r.Push<u16>(u16(session->service->PointerBufferSize()));
            break;
        default:
            LOG_ERROR("IPC", "Unknown control command {}", r.cmd);
            r.result = err::Unimplemented;
        }
        respond(false);
        return 0;
    }
    case 4:
    case 6: { // Request
        ServicePtr target = session->service;
        u64 payload = raw_addr;
        u32 payload_size = u32(raw_end > raw_addr ? raw_end - raw_addr : 0);
        const bool domain = session->domain != nullptr;
        if (domain) {
            const u8* dh = m.Ptr<u8>(raw_addr);
            r.is_domain = true;
            r.domain_cmd = dh[0];
            const u32 nobj = dh[1];
            payload_size = common::Read<u16>(dh + 2);
            r.object_id = common::Read<u32>(dh + 4);
            payload = raw_addr + 16;
            for (u32 i = 0; i < nobj; i++)
                r.in_objects.push_back(m.Read<u32>(payload + payload_size + i * 4));
            auto it = session->domain->objects.find(r.object_id);
            if (it == session->domain->objects.end()) {
                LOG_ERROR("IPC", "Domain object {} not found in {}", r.object_id, session->name);
                r.result = 0x1A01;
                respond(true);
                return 0;
            }
            target = it->second;
            if (r.domain_cmd == 2) { // close virtual handle
                LOG_DEBUG("IPC", "Domain close object {} ({})", r.object_id, target->Name());
                target->OnClose();
                session->domain->objects.erase(it);
                respond(true);
                return 0;
            }
        }
        const u32* hdr = m.Ptr<u32>(payload);
        if (hdr[0] != kSFCI)
            LOG_WARN("IPC", "{}: bad SFCI magic {:#x}", target->Name(), hdr[0]);
        r.cmd = hdr[2];
        if (payload_size > 16)
            r.args.assign(m.Ptr<u8>(payload + 16), m.Ptr<u8>(payload + payload_size));
        target->Dispatch(r);
        respond(domain);
        return 0;
    }
    default:
        LOG_ERROR("IPC", "Unsupported message type {} on {}", r.type, session->name);
        return kernel::rc::NotImplemented;
    }
}

} // namespace hle
