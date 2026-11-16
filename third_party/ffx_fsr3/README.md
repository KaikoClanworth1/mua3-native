# AMD FidelityFX Super Resolution 3.1 (upscaler)

Source of the FSR 3.1.6 temporal upscaler (no frame generation), from AMD's FidelityFX SDK at tag
[v2.4.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.4.0), unmodified. The paths below this
folder mirror `Kits/FidelityFX/` in that repository.

- `upscalers/fsr3/internal/shaders/`: the pass entry points (HLSL)
- `upscalers/fsr3/include/gpu/`: the passes' shader code, SPD and FSR 1 (RCAS)
- `api/internal/gpu/`: the shared FidelityFX GPU and CPU headers
- `upscalers/fsr3/internal/ffx_fsr3upscaler.cpp` and the headers: the host logic, a reference for the D3D11 port
  in `src/gpu/d3d11` (not compiled as is)

All of these files are under the MIT license (see `docs/license.md`, MIT section, and each file's header).
Copyright (c) Advanced Micro Devices, Inc. No AMD binaries (DLLs, prebuilt shader blobs) are included.
