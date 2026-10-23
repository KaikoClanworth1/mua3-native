#include "core/memory.h"

#include <windows.h>

namespace core {

static Memory* g_tracking_memory = nullptr;

// First write to a write-tracked page: mark it dirty, make it writable again and retry the instruction.
// Works for JIT code and host code alike (vectored handlers run before dynarmic's own fault handling).
static LONG CALLBACK WriteTrackingHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_tracking_memory)
        return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionInformation[0] != 1) // not a write
        return EXCEPTION_CONTINUE_SEARCH;
    const auto host = static_cast<uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1]);
    return g_tracking_memory->HandleWriteFault(host) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

Memory::Memory() {
    base_ = static_cast<u8*>(VirtualAlloc(nullptr, layout::kSpaceEnd, MEM_RESERVE, PAGE_NOACCESS));
    if (!base_)
        PANIC("Failed to reserve {} GB of address space for the guest", layout::kSpaceEnd >> 30);
    page_bits_ = std::vector<std::atomic<u64>>(size_t(layout::kSpaceEnd >> 12 >> 6));
    tracked_bits_ = std::vector<std::atomic<u64>>(page_bits_.size());
    for (auto& d : dirty_bits_)
        d = std::vector<std::atomic<u64>>(page_bits_.size());
    g_tracking_memory = this;
    AddVectoredExceptionHandler(1, WriteTrackingHandler);
    LOG_INFO("Memory", "Guest address space reserved at host {:p}", (void*)base_);
}

bool Memory::HandleWriteFault(uintptr_t host_addr) {
    const uintptr_t b = reinterpret_cast<uintptr_t>(base_);
    if (host_addr < b || host_addr >= b + layout::kSpaceEnd)
        return false;
    const u64 page = (host_addr - b) >> 12;
    const u64 bit = 1ull << (page & 63);
    auto& tw = tracked_bits_[size_t(page >> 6)];
    if (!(tw.load() & bit)) {
        // Another thread may have just unprotected it; if the page is writable now, retry.
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(base_ + (page << 12), &mbi, sizeof(mbi));
        return mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_READWRITE);
    }
    for (auto& d : dirty_bits_)
        d[size_t(page >> 6)].fetch_or(bit);
    DWORD old;
    VirtualProtect(base_ + (page << 12), 0x1000, PAGE_READWRITE, &old);
    tw.fetch_and(~bit);
    return true;
}

void Memory::TrackWrites(u64 addr, u64 size, u32 channel) {
    if (!size || !IsValid(addr, size))
        return;
    std::lock_guard lk(track_mutex_);
    const u64 first = addr >> 12, last = (addr + size - 1) >> 12;
    u64 run_start = 0;
    bool in_run = false;
    for (u64 p = first; p <= last + 1; p++) {
        bool need = false;
        if (p <= last) {
            const u64 bit = 1ull << (p & 63);
            dirty_bits_[channel][size_t(p >> 6)].fetch_and(~bit);
            need = !(tracked_bits_[size_t(p >> 6)].fetch_or(bit) & bit);
        }
        if (need && !in_run) {
            run_start = p;
            in_run = true;
        } else if (!need && in_run) {
            DWORD old;
            VirtualProtect(base_ + (run_start << 12), (p - run_start) << 12, PAGE_READONLY, &old);
            in_run = false;
        }
    }
}

bool Memory::ConsumeDirty(u64 addr, u64 size, u32 channel) {
    if (!size)
        return false;
    // 64 pages per word: mask the first and last words of the range.
    const u64 first = addr >> 12, last = (addr + size - 1) >> 12;
    for (u64 w = first >> 6; w <= last >> 6; w++) {
        u64 mask = ~0ull;
        if (w == first >> 6)
            mask &= ~0ull << (first & 63);
        if (w == last >> 6)
            mask &= ~0ull >> (63 - (last & 63));
        if (dirty_bits_[channel][size_t(w)].load(std::memory_order_relaxed) & mask)
            return true;
    }
    return false;
}

void Memory::MarkPages(u64 addr, u64 size, bool committed) {
    for (u64 p = addr >> 12, e = (addr + size + 0xFFF) >> 12; p < e; p++) {
        const u64 bit = 1ull << (p & 63);
        if (committed)
            page_bits_[size_t(p >> 6)].fetch_or(bit);
        else
            page_bits_[size_t(p >> 6)].fetch_and(~bit);
    }
}

Memory::~Memory() {
    if (base_)
        VirtualFree(base_, 0, MEM_RELEASE);
}

bool Memory::IsValid(u64 addr, u64 size) const {
    if (addr >= layout::kSpaceEnd || addr + size > layout::kSpaceEnd || addr + size < addr || !size)
        return size == 0 && addr < layout::kSpaceEnd;
    for (u64 p = addr >> 12, e = (addr + size - 1) >> 12; p <= e; p++)
        if (!(page_bits_[size_t(p >> 6)].load(std::memory_order_relaxed) & (1ull << (p & 63))))
            return false;
    return true;
}

std::string Memory::ReadString(u64 addr, size_t max) const {
    std::string s;
    if (!IsValid(addr))
        return s;
    for (size_t i = 0; i < max; i++) {
        const char c = char(base_[addr + i]);
        if (!c)
            break;
        s.push_back(c);
    }
    return s;
}

void Memory::SplitAt(u64 addr) {
    auto it = vmas_.upper_bound(addr);
    if (it == vmas_.begin())
        return;
    --it;
    const u64 start = it->first;
    if (start == addr || start + it->second.size <= addr)
        return;
    Vma tail = it->second;
    tail.size = start + it->second.size - addr;
    it->second.size = addr - start;
    vmas_.emplace(addr, tail);
}

void Memory::Insert(u64 addr, u64 size, u32 state, u32 perm, u32 attr) {
    SplitAt(addr);
    SplitAt(addr + size);
    vmas_.erase(vmas_.lower_bound(addr), vmas_.lower_bound(addr + size));
    if (state != MS_Free)
        vmas_.emplace(addr, Vma{size, state, perm, attr});
}

bool Memory::Map(u64 addr, u64 size, u32 state, u32 perm) {
    std::lock_guard lk(vma_mutex_);
    if (!size)
        return true;
    if (addr + size > layout::kSpaceEnd || (addr & 0xFFF) || (size & 0xFFF))
        return false;
    if (!VirtualAlloc(base_ + addr, size, MEM_COMMIT, PAGE_READWRITE)) {
        LOG_ERROR("Memory", "Commit failed at {:#x} size {:#x} (err {})", addr, size, GetLastError());
        return false;
    }
    Insert(addr, size, state, perm, 0);
    MarkPages(addr, size, true);
    used_ += size;
    return true;
}

bool Memory::Unmap(u64 addr, u64 size) {
    std::lock_guard lk(vma_mutex_);
    if (!size)
        return true;
    MarkPages(addr, size, false);
    for (u64 p = addr >> 12, e = (addr + size + 0xFFF) >> 12; p < e; p++) {
        const u64 bit = 1ull << (p & 63);
        tracked_bits_[size_t(p >> 6)].fetch_and(~bit);
        for (auto& d : dirty_bits_)
            d[size_t(p >> 6)].fetch_and(~bit);
    }
    VirtualFree(base_ + addr, size, MEM_DECOMMIT);
    Insert(addr, size, MS_Free, MP_None, 0);
    used_ = used_ > size ? used_ - size : 0;
    return true;
}

void Memory::SetState(u64 addr, u64 size, u32 state, u32 perm) {
    std::lock_guard lk(vma_mutex_);
    Insert(addr, size, state, perm, 0);
}

void Memory::SetPerm(u64 addr, u64 size, u32 perm) {
    std::lock_guard lk(vma_mutex_);
    SplitAt(addr);
    SplitAt(addr + size);
    for (auto it = vmas_.lower_bound(addr); it != vmas_.end() && it->first < addr + size; ++it)
        it->second.perm = perm;
}

void Memory::SetAttr(u64 addr, u64 size, u32 mask, u32 value) {
    std::lock_guard lk(vma_mutex_);
    SplitAt(addr);
    SplitAt(addr + size);
    for (auto it = vmas_.lower_bound(addr); it != vmas_.end() && it->first < addr + size; ++it)
        it->second.attr = (it->second.attr & ~mask) | (value & mask);
}

MemoryInfo Memory::Query(u64 addr) {
    std::lock_guard lk(vma_mutex_);
    MemoryInfo mi{};
    if (addr >= layout::kSpaceEnd) {
        mi.addr = layout::kSpaceEnd;
        mi.size = 0ull - layout::kSpaceEnd;
        mi.state = MS_Inaccessible;
        return mi;
    }
    auto it = vmas_.upper_bound(addr);
    if (it != vmas_.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second.size > addr) {
            mi.addr = prev->first;
            mi.size = prev->second.size;
            mi.state = prev->second.state;
            mi.perm = prev->second.perm;
            mi.attr = prev->second.attr;
            // Merge adjacent identical regions, like the kernel does.
            for (auto n = it; n != vmas_.end() && n->first == mi.addr + mi.size && n->second.state == mi.state &&
                              n->second.perm == mi.perm && n->second.attr == mi.attr;
                 ++n)
                mi.size += n->second.size;
            return mi;
        }
    }
    // Free gap
    u64 start = 0;
    if (it != vmas_.begin()) {
        auto prev = std::prev(it);
        start = prev->first + prev->second.size;
    }
    const u64 end = it == vmas_.end() ? layout::kSpaceEnd : it->first;
    mi.addr = start;
    mi.size = end - start;
    mi.state = MS_Free;
    return mi;
}

u64 Memory::FindFree(u64 region, u64 region_size, u64 size, u64 align) {
    std::lock_guard lk(vma_mutex_);
    u64 addr = common::AlignUp(region, align);
    const u64 end = region + region_size;
    while (addr + size <= end) {
        auto it = vmas_.upper_bound(addr);
        if (it != vmas_.begin()) {
            auto prev = std::prev(it);
            if (prev->first + prev->second.size > addr) {
                addr = common::AlignUp(prev->first + prev->second.size, align);
                continue;
            }
        }
        if (it == vmas_.end() || it->first >= addr + size)
            return addr;
        addr = common::AlignUp(it->first + it->second.size, align);
    }
    return 0;
}

bool Memory::SetHeapSize(u64 size) {
    std::lock_guard lk(vma_mutex_);
    if (size > layout::kHeapSize)
        return false;
    if (size > heap_size_) {
        if (!Map(layout::kHeapBase + heap_size_, size - heap_size_, MS_Normal, MP_RW))
            return false;
    } else if (size < heap_size_) {
        Unmap(layout::kHeapBase + size, heap_size_ - size);
    }
    heap_size_ = size;
    return true;
}

} // namespace core
