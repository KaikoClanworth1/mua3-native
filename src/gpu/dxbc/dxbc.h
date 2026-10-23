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

enum class Stage { Compute };

// Compiles `source` (entry point `main`) for `stage`. On failure returns false and sets `err`.
bool CompileHlsl(const std::string& source, Stage stage, std::vector<u8>& out, std::string& err);

// The DXBC container checksum: MD5 variant over everything after the 20-byte header prefix.
void Checksum(const u8* blob, size_t size, u32 out[4]);

// Wraps a SHEX token stream in a container (empty input/output signatures).
std::vector<u8> Container(const std::vector<u32>& shex);

} // namespace gpu::dxbc
