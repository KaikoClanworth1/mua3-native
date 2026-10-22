#pragma once
#include <memory>
#include <mutex>
#include <unordered_map>

#include "common/common.h"

namespace hle::nv {

// nvmap handle registry, shared between nvdrv and vi (buffer queue graphic buffers reference nvmap ids).
struct NvMap {
    struct Object {
        u32 id = 0, handle = 0, size = 0, align = 0;
        u8 kind = 0;
        u64 addr = 0;
        bool allocated = false;
    };
    u32 Create(u32 size);
    std::shared_ptr<Object> Get(u32 handle);
    std::shared_ptr<Object> GetById(u32 id);

    std::mutex mutex;
    std::unordered_map<u32, std::shared_ptr<Object>> by_handle, by_id;
    u32 next_id = 1, next_handle = 1;
};

NvMap& Map();

} // namespace hle::nv
