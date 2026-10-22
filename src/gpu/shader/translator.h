#pragma once
#include <set>
#include <string>
#include <vector>

#include "common/common.h"

namespace gpu::shader {

enum class Stage { Vertex, Fragment, Geometry, Compute };

enum class TexDim : u8 { Tex1D, Tex2D, Tex3D, Cube };

struct TextureUse {
    u32 cbuf_offset = 0; // byte offset of the handle in the texture constant buffer
    TexDim dim = TexDim::Tex2D;
    bool array = false;
    bool shadow = false; // depth compare
    bool is_int = false;
    u32 slot = 0;        // HLSL t/s register
};

struct TranslateResult {
    bool ok = false;
    std::string hlsl;
    std::string error;
    std::vector<TextureUse> textures;
    u32 cbuf_mask = 0;           // constant buffers referenced
    u32 attrib_in_mask = 0;      // VS: generic inputs used
    u32 rt_mask = 0;             // PS: render targets written (bit per RT)
    bool writes_depth = false;
    bool uses_loop = false;
    std::set<std::string> unimplemented;
};

struct TranslateOptions {
    bool force_lod0 = false; // use SampleLevel(0) everywhere (for shaders the HLSL compiler rejects otherwise)
};

// `code` points at the shader program header (SPH, 0x50 bytes) followed by instructions; `size` in bytes.
TranslateResult Translate(Stage stage, const u8* code, size_t size, const TranslateOptions& opt = {});

// Finds the program length (bytes, including the SPH) by scanning for the end-of-program self branch.
size_t ProgramSize(const u8* code, size_t max_size);

} // namespace gpu::shader
