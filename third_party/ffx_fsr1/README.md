AMD FidelityFX Super Resolution 1.0 (FSR 1): the EASU upscaler and RCAS sharpener shader headers, unmodified, from
https://github.com/GPUOpen-Effects/FidelityFX-FSR (ffx-fsr/ffx_a.h, ffx-fsr/ffx_fsr1.h). MIT license: LICENSE.txt.
src/CMakeLists.txt embeds them for src/gpu/d3d11/fsr.cpp, which compiles them at runtime.
