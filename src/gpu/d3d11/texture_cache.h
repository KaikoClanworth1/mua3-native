#pragma once
#include <array>
#include <memory>
#include <unordered_map>

#include <d3d11.h>
#include <wrl/client.h>

#include "gpu/shader/translator.h"

namespace gpu {

using Microsoft::WRL::ComPtr;
class D3D11Renderer;

struct TicEntry {
    std::array<u32, 8> w{};
    u32 Format() const { return w[0] & 0x7F; }
    u32 RType() const { return (w[0] >> 7) & 7; }
    u32 Swizzle() const { return (w[0] >> 19) & 0xFFF; } // x,y,z,w sources, 3 bits each
    u64 Address() const { return (u64(w[2] & 0xFFFF) << 32) | w[1]; }
    u32 HeaderVersion() const { return (w[2] >> 21) & 7; }
    u32 BlockHeight() const { return (w[3] >> 3) & 7; }
    u32 BlockDepth() const { return (w[3] >> 6) & 7; }
    u32 MaxMip() const { return (w[3] >> 28) & 0xF; }
    u32 Pitch() const { return (w[3] & 0xFFFF) << 5; }
    u32 Width() const { return (w[4] & 0xFFFF) + 1; }
    bool Srgb() const { return (w[4] >> 22) & 1; }
    u32 Type() const { return (w[4] >> 23) & 0xF; }
    u32 Height() const { return (w[5] & 0xFFFF) + 1; }
    u32 Depth() const { return ((w[5] >> 16) & 0x3FFF) + 1; }
};

struct CachedTexture {
    u64 key = 0;
    u64 gpu_va = 0, size = 0;
    std::vector<std::pair<u64, u64>> cpu_ranges; // guest CPU ranges backing it (write-tracked)
    ComPtr<ID3D11Resource> res;
    ComPtr<ID3D11ShaderResourceView> srv;
    u64 last_use = 0;
};

class TextureCache {
public:
    TextureCache(ID3D11Device* dev, ID3D11DeviceContext* ctx, D3D11Renderer& r);
    // Returns an SRV for the TIC entry (or null). `swizzle_out` receives the packed component swizzle.
    ID3D11ShaderResourceView* Get(const TicEntry& tic, const shader::TextureUse& use, u32& swizzle_out);
    ID3D11SamplerState* GetSampler(const u32* tsc, bool compare);
    void Invalidate(u64 gpu_va, u64 size);
    void NextFrame() { frame_++; }
    u64 reuploads = 0;

private:
    ID3D11ShaderResourceView* Upload(const TicEntry& tic, const shader::TextureUse& use, u64 key);
    ID3D11Device* dev_;
    ID3D11DeviceContext* ctx_;
    D3D11Renderer& r_;
    std::unordered_map<u64, std::unique_ptr<CachedTexture>> textures_;
    std::unordered_map<u64, ComPtr<ID3D11SamplerState>> samplers_;
    ID3D11ShaderResourceView* Fallback(const shader::TextureUse& use);
    ComPtr<ID3D11ShaderResourceView> fallback_;
    ComPtr<ID3D11ShaderResourceView> fallbacks_[4]; // 2D, 3D, cube, 2D array
    u64 frame_ = 0;
};

} // namespace gpu
