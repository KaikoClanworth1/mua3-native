#pragma once
// Direct DXBC (shader model 5) generation, bypassing FXC.
//
// The shader translator emits a small, regular HLSL dialect. CompileHlsl compiles that dialect straight to DXBC
// bytecode without optimisation (the GPU driver optimises anyway), which takes milliseconds where FXC can take
// tens of minutes on large programs. Anything outside the dialect makes it fail with an error, and callers fall
// back to FXC.
#include <string>
#include <vector>

#include "common/common.h"

namespace gpu::dxbc {

enum class Stage { Compute, Vertex, Pixel };

// Compiles `source` (entry point `main`) for `stage`. On failure returns false and sets `err`.
bool CompileHlsl(const std::string& source, Stage stage, std::vector<u8>& out, std::string& err);

// The DXBC container checksum: MD5 variant over everything after the 20-byte header prefix.
void Checksum(const u8* blob, size_t size, u32 out[4]);

// One input or output signature element (ISGN / OSGN), as FXC writes them.
struct SigElement {
    std::string name;  // semantic name ("SV_Position", "ATTR", "SV_Target", ...)
    u32 index = 0;     // semantic index
    u32 sys_value = 0; // D3D_NAME: 0 none (also SV_Target / SV_Depth), 1 position, 6 vertex id, 8 instance id, 9 front face
    u32 comp_type = 3; // 1 uint, 2 sint, 3 float
    u32 reg = 0;       // register (0xFFFFFFFF: oDepth)
    u8 mask = 0xF;
    u8 rw_mask = 0;    // inputs: components read; outputs: components never written
};

// Wraps a SHEX token stream in a container with the given signatures (empty for compute).
std::vector<u8> Container(const std::vector<u32>& shex, const std::vector<SigElement>& isgn = {},
                          const std::vector<SigElement>& osgn = {});

} // namespace gpu::dxbc
