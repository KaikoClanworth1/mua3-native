#pragma once
// Global exclusive monitor for recompiled LDXR/STXR (same semantics as dynarmic's, without its x64 spin lock, which
// is generated at run time with Xbyak): each core holds at most one reservation (address + the value it read); a
// store-exclusive succeeds if the core still holds a reservation on that address, and clears every core's
// reservation on it. The store itself is a compare-and-swap against the value read, as on dynarmic's fastmem path.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <type_traits>
#include <vector>

namespace cpu {

class NativeExclusiveMonitor {
public:
    using Vector = std::array<std::uint64_t, 2>;
    explicit NativeExclusiveMonitor(size_t processors)
        : addresses_(processors, kInvalid), values_(processors) {}

    template <typename T, typename Function>
    T ReadAndMark(size_t processor, std::uint64_t address, Function op) {
        static_assert(std::is_trivially_copyable_v<T>);
        Lock();
        addresses_[processor] = address;
        const T value = op();
        values_[processor] = {};
        std::memcpy(values_[processor].data(), &value, sizeof(T));
        Unlock();
        return value;
    }

    template <typename T, typename Function>
    bool DoExclusiveOperation(size_t processor, std::uint64_t address, Function op) {
        static_assert(std::is_trivially_copyable_v<T>);
        Lock();
        if (addresses_[processor] != address) {
            Unlock();
            return false;
        }
        for (auto& a : addresses_)
            if (a == address)
                a = kInvalid;
        T saved;
        std::memcpy(&saved, values_[processor].data(), sizeof(T));
        const bool result = op(saved);
        Unlock();
        return result;
    }

    void ClearProcessor(size_t processor) {
        Lock();
        addresses_[processor] = kInvalid;
        Unlock();
    }
    void Clear() {
        Lock();
        for (auto& a : addresses_)
            a = kInvalid;
        Unlock();
    }

private:
    static constexpr std::uint64_t kInvalid = 0xDEADDEADDEADDEADull;
    void Lock() {
        for (int spins = 0; lock_.exchange(true, std::memory_order_acquire); spins++)
            if (spins > 64)
                std::this_thread::yield();
    }
    void Unlock() { lock_.store(false, std::memory_order_release); }

    std::atomic<bool> lock_{false};
    std::vector<std::uint64_t> addresses_;
    std::vector<Vector> values_;
};

} // namespace cpu
