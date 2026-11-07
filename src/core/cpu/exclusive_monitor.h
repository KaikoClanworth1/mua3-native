#pragma once
// Exclusive monitor for recompiled LDXR/STXR. Each core holds at most one reservation: the address and the value it
// read. A store-exclusive succeeds if the core still holds a reservation on that address and memory still holds the
// value it read: the store itself is a compare-and-swap against that value, as on dynarmic's fastmem path.
//
// There is no global lock and no clearing of other cores' reservations (dynarmic's monitor does both under a spin
// lock, which every guest mutex paid for). Another core's successful store changes the value, so this core's
// compare-and-swap fails as it would on ARM. The one difference: a store that writes back the very value it read,
// or an A-B-A sequence between our load and store, does not fail ours.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

namespace cpu {

class NativeExclusiveMonitor {
public:
    using Vector = std::array<std::uint64_t, 2>;
    explicit NativeExclusiveMonitor(size_t processors) : count_(processors), slots_(new Slot[processors]) {}

    template <typename T, typename Function>
    T ReadAndMark(size_t processor, std::uint64_t address, Function op) {
        static_assert(std::is_trivially_copyable_v<T>);
        Slot& s = slots_[processor];
        s.address.store(address, std::memory_order_relaxed);
        const T value = op();
        s.value = {};
        std::memcpy(s.value.data(), &value, sizeof(T));
        return value;
    }

    template <typename T, typename Function>
    bool DoExclusiveOperation(size_t processor, std::uint64_t address, Function op) {
        static_assert(std::is_trivially_copyable_v<T>);
        Slot& s = slots_[processor];
        if (s.address.load(std::memory_order_relaxed) != address)
            return false;
        s.address.store(kInvalid, std::memory_order_relaxed);
        T saved;
        std::memcpy(&saved, s.value.data(), sizeof(T));
        return op(saved);
    }

    void ClearProcessor(size_t processor) { slots_[processor].address.store(kInvalid, std::memory_order_relaxed); }
    void Clear() {
        for (size_t i = 0; i < count_; i++)
            slots_[i].address.store(kInvalid, std::memory_order_relaxed);
    }

private:
    static constexpr std::uint64_t kInvalid = 0xDEADDEADDEADDEADull;
    struct alignas(64) Slot { // one cache line per core: cores never share a line
        std::atomic<std::uint64_t> address{kInvalid};
        Vector value{};
    };
    size_t count_;
    std::unique_ptr<Slot[]> slots_;
};

} // namespace cpu
