#include "gpu/d3d11/texture_cache.h"

#include <set>

#include "gpu/d3d11/renderer_d3d11.h"
#include "gpu/d3d11/shader_cache.h"
#include "gpu/formats.h"
#include "gpu/gpu.h"

namespace gpu {

TextureCache::TextureCache(ID3D11Device* dev, ID3D11DeviceContext* ctx, D3D11Renderer& r)
    : dev_(dev), ctx_(ctx), r_(r) {
    // 1x1 opaque grey used when a texture can't be decoded.
    D3D11_TEXTURE2D_DESC td{1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE,
                            D3D11_BIND_SHADER_RESOURCE, 0, 0};
    const u32 px = 0xFF808080;
    D3D11_SUBRESOURCE_DATA sd{&px, 4, 4};
    ComPtr<ID3D11Texture2D> t;
    dev_->CreateTexture2D(&td, &sd, &t);
    dev_->CreateShaderResourceView(t.Get(), nullptr, &fallback_);
}

void TextureCache::Invalidate(u64 gpu_va, u64 size) {
    for (auto it = textures_.begin(); it != textures_.end();) {
        auto& t = *it->second;
        if (t.gpu_va < gpu_va + size && gpu_va < t.gpu_va + t.size)
            it = textures_.erase(it);
        else
            ++it;
    }
}

ID3D11ShaderResourceView* TextureCache::Fallback(const shader::TextureUse& use) {
    const int kind = use.dim == shader::TexDim::Tex3D ? 1 : use.dim == shader::TexDim::Cube ? 2 : use.array ? 3 : 0;
    if (fallbacks_[kind])
        return fallbacks_[kind].Get();
    const u32 px[6] = {0xFF808080, 0xFF808080, 0xFF808080, 0xFF808080, 0xFF808080, 0xFF808080};
    D3D11_SUBRESOURCE_DATA sd[6];
    for (auto& d : sd)
        d = {px, 4, 4};
    if (kind == 1) {
        D3D11_TEXTURE3D_DESC td{1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ComPtr<ID3D11Texture3D> t;
        dev_->CreateTexture3D(&td, sd, &t);
        dev_->CreateShaderResourceView(t.Get(), nullptr, &fallbacks_[kind]);
    } else {
        const UINT layers = kind == 2 ? 6 : 1;
        D3D11_TEXTURE2D_DESC td{1, 1, 1, layers, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE,
                                D3D11_BIND_SHADER_RESOURCE, 0, kind == 2 ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u};
        ComPtr<ID3D11Texture2D> t;
        dev_->CreateTexture2D(&td, sd, &t);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (kind == 2) sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE, sv.TextureCube = {0, 1};
        else if (kind == 3) sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY, sv.Texture2DArray = {0, 1, 0, 1};
        else sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, sv.Texture2D = {0, 1};
        dev_->CreateShaderResourceView(t.Get(), &sv, &fallbacks_[kind]);
    }
    return fallbacks_[kind].Get();
}

ID3D11ShaderResourceView* TextureCache::Get(const TicEntry& tic, const shader::TextureUse& use, u32& swizzle) {
    // Pack x/y/z/w component sources (3 bits each) for the shader's ApplySwizzle.
    swizzle = tic.Swizzle();
    const u64 addr = tic.Address();
    if (use.dim == shader::TexDim::Tex3D) {
        static int n = 0;
        if (n++ < 4)
            LOG_INFO("Texture", "3D use: tic addr {:#x} type {} fmt {:#x} {}x{}x{} surface {}", addr, tic.Type(), tic.Format(),
                     tic.Width(), tic.Height(), tic.Depth(), (void*)r_.FindSurfaceByGpu(addr));
    }
    if (Surface* s = r_.FindSurfaceByGpu(addr); s && s->srv && use.dim == shader::TexDim::Tex2D && !use.array) {
        s->last_use = frame_;
        if (s->depth)
            swizzle = 0xB1A; // depth samples come back in .x
        return s->srv.Get();
    }
    const u64 key = Hash64(tic.w.data(), 32) ^ (u64(use.dim) << 56) ^ (u64(use.array) << 60);
    auto it = textures_.find(key);
    if (it != textures_.end()) {
        it->second->last_use = frame_;
        return it->second->srv.Get();
    }
    auto* srv = Upload(tic, use, key);
    return srv ? srv : Fallback(use);
}

ID3D11ShaderResourceView* TextureCache::Upload(const TicEntry& tic, const shader::TextureUse& use, u64 key) {
    const TexFormatInfo fi = TicFormat(tic.Format(), tic.RType(), tic.Srgb());
    if (fi.dxgi == DXGI_FORMAT_UNKNOWN) {
        static std::set<u32> warned;
        if (warned.insert(tic.Format()).second)
            LOG_WARN("Texture", "Unsupported texture format {:#x} (type {})", tic.Format(), tic.RType());
        return nullptr;
    }
    const u32 type = tic.Type();
    const u32 width = tic.Width(), height = (type == 0 || type == 4 || type == 6) ? 1 : tic.Height();
    u32 depth = 1, layers = 1;
    if (type == 2)
        depth = tic.Depth();
    else if (type == 4 || type == 5)
        layers = tic.Depth();
    else if (type == 3)
        layers = 6;
    else if (type == 8)
        layers = tic.Depth() * 6;
    const u32 mips = type == 7 ? 1 : std::min<u32>(tic.MaxMip() + 1, 16);
    const bool block_linear = tic.HeaderVersion() >= 3;
    const u32 bpb = fi.bytes_per_block;

    // Compute per-level layout
    struct Level {
        u32 w, h, d, bw, bh, bhl, bdl;
        u64 offset, size;
    };
    std::vector<Level> levels;
    u64 layer_size = 0;
    for (u32 m = 0; m < mips; m++) {
        Level l{};
        l.w = std::max(1u, width >> m);
        l.h = std::max(1u, height >> m);
        l.d = std::max(1u, depth >> m);
        l.bw = (l.w + fi.block_w - 1) / fi.block_w;
        l.bh = (l.h + fi.block_h - 1) / fi.block_h;
        l.bhl = tic.BlockHeight();
        l.bdl = tic.BlockDepth();
        while (l.bhl > 0 && (8u << (l.bhl - 1)) >= l.bh)
            l.bhl--;
        while (l.bdl > 0 && (1u << (l.bdl - 1)) >= l.d)
            l.bdl--;
        l.offset = layer_size;
        l.size = block_linear ? BlockLinearSize(l.bw, l.bh, l.d, bpb, l.bhl, l.bdl) : u64(tic.Pitch()) * l.bh * l.d;
        layer_size += l.size;
        levels.push_back(l);
    }
    if (block_linear && layers > 1)
        layer_size = common::AlignUp(layer_size, 512ull << (tic.BlockHeight() + tic.BlockDepth()));
    const u64 total = layer_size * layers;
    if (total == 0 || total > (512ull << 20)) {
        LOG_WARN("Texture", "Texture too large or empty ({} bytes)", total);
        return nullptr;
    }
    std::vector<u8> guest(static_cast<size_t>(total));
    G().ReadGpu(tic.Address(), guest.data(), guest.size());

    // Decode every subresource
    std::vector<std::vector<u8>> sub;
    std::vector<D3D11_SUBRESOURCE_DATA> init;
    const bool astc = fi.astc;
    for (u32 layer = 0; layer < layers; layer++) {
        for (u32 m = 0; m < mips; m++) {
            const Level& l = levels[m];
            const u8* src = guest.data() + layer * layer_size + l.offset;
            const u32 row = l.bw * bpb;
            std::vector<u8> lin(size_t(row) * l.bh * l.d);
            if (block_linear)
                Deswizzle(src, lin.data(), l.bw, l.bh, l.d, bpb, l.bhl, l.bdl, row);
            else
                for (u32 y = 0; y < l.bh * l.d; y++)
                    std::memcpy(&lin[size_t(y) * row], src + size_t(y) * tic.Pitch(), row);
            if (astc) {
                std::vector<u8> rgba(size_t(l.w) * l.h * 4);
                DecodeAstc(lin.data(), rgba.data(), l.w, l.h, fi.block_w, fi.block_h);
                sub.push_back(std::move(rgba));
                init.push_back({sub.back().data(), l.w * 4, l.w * l.h * 4});
            } else {
                sub.push_back(std::move(lin));
                init.push_back({sub.back().data(), row, row * l.bh});
            }
        }
    }
    // D3D requires the init array pointers to stay valid; `sub` owns the storage.
    for (size_t i = 0; i < init.size(); i++)
        init[i].pSysMem = sub[i].data();

    auto tex = std::make_unique<CachedTexture>();
    tex->key = key;
    tex->gpu_va = tic.Address();
    tex->size = total;
    tex->last_use = frame_;
    HRESULT hr;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fi.dxgi;
    if (type == 2 && use.dim == shader::TexDim::Tex3D) {
        D3D11_TEXTURE3D_DESC td{width, height, depth, mips, fi.dxgi, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ComPtr<ID3D11Texture3D> t;
        hr = dev_->CreateTexture3D(&td, init.data(), &t);
        tex->res = t;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = mips;
    } else {
        const bool cube = use.dim == shader::TexDim::Cube && layers % 6 == 0;
        D3D11_TEXTURE2D_DESC td{width, height, mips, layers, fi.dxgi, {1, 0}, D3D11_USAGE_IMMUTABLE,
                                D3D11_BIND_SHADER_RESOURCE, 0, cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u};
        ComPtr<ID3D11Texture2D> t;
        hr = dev_->CreateTexture2D(&td, init.data(), &t);
        tex->res = t;
        if (cube) {
            if (use.array) {
                sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
                sv.TextureCubeArray = {0, mips, 0, layers / 6};
            } else {
                sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
                sv.TextureCube = {0, mips};
            }
        } else if (use.array || layers > 1) {
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sv.Texture2DArray = {0, mips, 0, layers};
            if (!use.array)
                sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, sv.Texture2D = {0, mips};
        } else {
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D = {0, mips};
        }
    }
    if (FAILED(hr) || !tex->res) {
        LOG_WARN("Texture", "CreateTexture failed {:#x} ({}x{} fmt {:#x} mips {} layers {})", u32(hr), width, height,
                 tic.Format(), mips, layers);
        return nullptr;
    }
    if (FAILED(dev_->CreateShaderResourceView(tex->res.Get(), &sv, &tex->srv)))
        return nullptr;
    auto* srv = tex->srv.Get();
    textures_[key] = std::move(tex);
    return srv;
}

ID3D11SamplerState* TextureCache::GetSampler(const u32* tsc, bool compare) {
    const u64 key = Hash64(tsc, 32) ^ (compare ? 1ull : 0ull);
    auto it = samplers_.find(key);
    if (it != samplers_.end())
        return it->second.Get();
    auto wrap = [](u32 m) {
        switch (m) {
        case 0: return D3D11_TEXTURE_ADDRESS_WRAP;
        case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case 3: case 6: return D3D11_TEXTURE_ADDRESS_BORDER;
        case 5: case 7: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
        default: return D3D11_TEXTURE_ADDRESS_CLAMP;
        }
    };
    D3D11_SAMPLER_DESC d{};
    d.AddressU = wrap(tsc[0] & 7);
    d.AddressV = wrap((tsc[0] >> 3) & 7);
    d.AddressW = wrap((tsc[0] >> 6) & 7);
    const u32 aniso = (tsc[0] >> 20) & 7;
    const bool mag_lin = (tsc[1] & 3) == 2, min_lin = ((tsc[1] >> 4) & 3) == 2;
    const u32 mip = (tsc[1] >> 6) & 3;
    const bool mip_lin = mip == 3;
    if (aniso > 0 && min_lin && mag_lin) {
        d.Filter = D3D11_FILTER_ANISOTROPIC;
        d.MaxAnisotropy = std::min(16u, 1u << aniso);
    } else {
        u32 f = (min_lin ? 0x10 : 0) | (mag_lin ? 0x4 : 0) | (mip_lin ? 0x1 : 0);
        d.Filter = D3D11_FILTER(f);
    }
    if (compare) {
        d.Filter = D3D11_FILTER(d.Filter | 0x80);
        static const D3D11_COMPARISON_FUNC fn[] = {D3D11_COMPARISON_NEVER, D3D11_COMPARISON_LESS, D3D11_COMPARISON_EQUAL,
                                                   D3D11_COMPARISON_LESS_EQUAL, D3D11_COMPARISON_GREATER,
                                                   D3D11_COMPARISON_NOT_EQUAL, D3D11_COMPARISON_GREATER_EQUAL,
                                                   D3D11_COMPARISON_ALWAYS};
        d.ComparisonFunc = fn[(tsc[0] >> 10) & 7];
    } else {
        d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    }
    const s32 bias = s32(((tsc[1] >> 12) & 0x1FFF) << 19) >> 19;
    d.MipLODBias = bias / 256.0f;
    d.MinLOD = (tsc[2] & 0xFFF) / 256.0f;
    d.MaxLOD = mip == 1 ? 0.0f : ((tsc[2] >> 12) & 0xFFF) / 256.0f;
    std::memcpy(d.BorderColor, &tsc[4], 16);
    ComPtr<ID3D11SamplerState> s;
    dev_->CreateSamplerState(&d, &s);
    samplers_[key] = s;
    return s.Get();
}

} // namespace gpu
