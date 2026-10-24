# Native renderer plan: replacing GPU emulation with NVN-level calls

## Where we are

- The game draws through **NVN**, Nintendo's graphics API. `main` resolves 513 NVN entry points by name at start-up
  (`nvnBootstrapLoader("nvnDeviceGetProcAddress")`, then one `nvnDeviceGetProcAddress` call per function, the
  standard `nvnLoadCProcs` pattern). The list is in `nvn_functions_main.txt`. By family: CommandBuffer 144,
  Texture 96, Sampler 48, Queue 34, Device 30, Window 24, Multisample 23, Memory(pool) 18, Buffer 18, Blend 15,
  Depth/stencil 13, Vertex 10, Polygon 9, Program 7, Event 7, Color 7, Sync 6, Channel 3.
- The NVN *driver* is in the `sdk` module (it exports `nvnBootstrapLoader`). Today it runs as recompiled guest code
  and turns the API calls into Maxwell GPU command buffers. Those go through nvdrv ioctls to `src/gpu`, which
  decodes the method stream back into D3D11 state (`renderer_d3d11.cpp`). Shaders are Maxwell machine code,
  translated to HLSL (`src/gpu/shader`) and compiled to DXBC. They are now prewarmed at boot from `shader_dump/`,
  so gameplay never compiles a shader.

## Target

Answer the game's NVN calls with host code that drives D3D11 (or D3D12) directly. Command-buffer encoding by the
sdk driver and decoding by `src/gpu` both disappear. Shaders stay Maxwell binaries inside the game data, so the
translator stays; it just runs ahead of time.

## Mechanism

1. **Hook point**: HLE `nvnBootstrapLoader` (sdk export, resolved by rtld) so it returns a host
   `nvnDeviceGetProcAddress`, and that returns per-name addresses in a reserved guest range (one 4-byte slot per
   function, e.g. a `BRK #0xnvn` page).
2. **Fast calls**: the game calls NVN through function pointers (`BLR`). In recompiled code those go through
   `recomp::Dispatch`, which looks the target up in the native table. Register the slot addresses there, with host
   functions that read their arguments from `State` (x0..x7, v0..v7 per AAPCS64) and write x0. JIT-side calls hit the
   BRK slot and trap the same way. No SVCs and no guest code for the stubs.
3. **Objects**: NVN objects are caller-allocated structs in guest memory, opaque to the game (`NVNdevice` 0x3000
   bytes, `NVNcommandBuffer` 0xA0, `NVNtexture` 0xC0, ...). Keep a host object per guest address in a hash map;
   the guest struct only needs a handle.
4. **Memory pools**: NVN buffers and textures live in app-allocated memory pools (`nvnMemoryPoolInitialize` with a
   guest pointer). Map each pool to D3D resources: a buffer view per `nvnBufferInitialize`, and a texture per
   `nvnTextureInitialize` at (pool, offset). The guest CPU writes pools directly (vertex data, constants, texture
   uploads via buffer copies), so reuse the existing write-tracking dirty pages to know what to re-upload, as the
   current vertex/constant caches already do.
5. **Command buffers**: record `nvnCommandBuffer*` calls into a host command list (draws, binds, state objects,
   copies, barriers). `nvnQueueSubmitCommands` replays the lists on the D3D context. Command memory and control
   memory supplied by the app are ignored.
6. **Programs**: `nvnProgramSetShaders` gives Maxwell code addresses: hash, then translate with the existing
   translator (or take the prewarmed result). Bindings and uniform buffers map as they do now.
7. **Sync, Window and present**: `nvnQueueFenceSync`, `nvnSyncWait`, `nvnWindowAcquireTexture` and
   `nvnQueuePresentTexture` map onto the existing display/present path, replacing the vi/nvdrv fence plumbing.
8. **Rollout**: keep the current GPU emulation behind a switch (`MUA3_NVN_HLE=0`). Bring the HLE path up family by
   family, comparing frames against the emulated path (the RT dump tools exist).

## Census (step 1, done 2026-10-24)

`MUA3_NVN_CENSUS=1` counts every indirect call made by recompiled code and records the pointer each
`nvnDeviceGetProcAddress(device, "name")` call returns. Results go to `nvn_census.txt` (raw, every 10 s); the
ST010 analysis is in `nvn_census_st010.txt`. The pointer table is at main `0xcfc3d38` (512 slots). Note that the
names in `main`'s strings are *not* in table order.

- 519 names resolved, **176 functions used at all, 124 called every frame in gameplay**, 52 only at
  boot/menus/loading, 343 never.
- Per frame in ST010: texture handle/bind ~3,200 (`nvnTexturePoolRegisterTexture`, `nvnDeviceGetTextureHandle`,
  `nvnCommandBufferBindTexture`), vertex stream state ~2,900, `nvnBufferGetAddress` ~2,700, uniform buffer binds
  ~1,300, storage buffer binds ~790, `nvnCommandBufferDrawElementsBaseVertex` ~640, `DrawElementsIndirect` ~360,
  program binds ~370, barriers ~190, blend/colour/channel-mask state ~80, compute dispatches ~50.
- So the native path must handle: indexed and indirect draws, ~3k texture binds per frame through texture/sampler
  pools and handles, vertex stream/attribute state objects, uniform/storage buffers by GPU address, blend/colour/
  depth state objects, render-enable conditionals (predicated draws), barriers and compute dispatch.

## Recorded frame: argument layouts (MUA3_NVN_RECORD=1 MUA3_NVN_DUMP=<seconds>)

Every `nvnCommandBuffer*` call and every state-object setter now goes through a generic recording wrapper
(`Generic<I>` in `nvn_hle.cpp`, one slot per resolved name, x0-x7 and v0-v3 recorded before the driver runs; state
setters update a host shadow per object). `MUA3_NVN_DUMP=<seconds>` writes the first large submit after that time,
with the state objects it binds, to `nvn_frame.txt`. One ST010 frame: 39 command types, ~4,700 commands
(`nvn_frame_st010_commands.txt`). Layouts confirmed from real values:

| Command | Arguments after the command buffer |
|---|---|
| SetRenderTargets | numColors, NVNtexture** colors, NVNtextureView** colorViews, NVNtexture* depth, depthView |
| ClearColor | index, const float* rgba, channel mask |
| ClearDepthStencil | depth (v0), depthMask, stencil, stencilMask |
| SetViewports / SetScissors / SetDepthRanges | first, count, pointer to float[4] / int[4] / float[2] arrays |
| BindProgram | NVNprogram*, stage bits (0x20 compute; graphics bits below) |
| BindVertexAttribState / BindVertexStreamState | count, pointer to the state array |
| BindVertexBuffer | index, GPU address, size |
| BindUniformBuffer / BindStorageBuffer | stage (0 VS ... 5 CS), index, GPU address, size |
| BindTexture | stage, index, NVN handle (tex id \| sampler id << 20 \| 1 << 32) |
| DrawElementsBaseVertex | mode (4 triangles, 5 strip), index type (1 u16, 2 u32), count, index GPU address, base vertex |
| DrawElementsIndirect | mode, index type, index GPU address, indirect GPU address |
| DrawArrays | mode, first, count |
| SetRenderEnable / SetRenderEnableConditional | enable / mode, counter GPU address (predicated draws) |
| Barrier | barrier bits |
| DispatchCompute | x, y, z |
| Bind{Blend,Color,ChannelMask,Polygon,DepthStencil,Multisample}State | state object pointer (shadowed) |
| SetStencilRef / SetStencilMask / SetStencilValueMask | faces, value |
| SetPolygonOffsetClamp | factor, units, clamp (v0..v2) |
| SetTexturePool / SetSamplerPool | pool object |

**Learned conventions** (`MUA3_NVN_RECORD=1 MUA3_NVN_DUMP=150 MUA3_NVN_LEARN=1`: the emulated path logs how it
resolves surfaces, texture handles and constant buffers during that frame; all samples agree):
- Uniform buffers: NVN `BindUniformBuffer(stage, i, ...)` is hardware constant buffer **i + 3**.
- Texture handles: hardware constant buffer **2**, offset **0x20 + i * 8** for NVN `BindTexture(stage, i, ...)`.
- Stages: NVN stage 0 (vertex) is hardware stage 0 (vertex B), NVN stage 1 (fragment) is hardware stage 4.
- Texture GPU addresses: the pool's buffer address (`nvnMemoryPoolGetBufferAddress`) + offset + **0xC3C00000**
  for the main pool (textures use a separate mapping; derive this per pool).
- Shader programs: KTGL binds separable single-stage programs (BindProgram stage bits 1 vertex, 2 fragment, ...,
  0x20 compute). ProgramSetShaders' code address points at an NVN blob (magic 0x12345678); the Maxwell program
  header (SPH) is always at **code + 0x30**, so the existing shader cache (and prewarm) applies as is.
- NVN format enum (GL ordering; every learned texture format and the vertex formats seen fit it):
  0x01 R8, 0x02 R8SN, 0x03 R8UI, 0x04 R8I, 0x05 R16F, 0x06 R16, 0x07 R16SN, 0x08 R16UI, 0x09 R16I, 0x0A R32F,
  0x0B R32UI, 0x0C R32I, 0x0D RG8, 0x0E RG8SN, 0x0F RG8UI, 0x10 RG8I, 0x11 RG16F, 0x12 RG16, 0x13 RG16SN,
  0x14 RG16UI, 0x15 RG16I, 0x16 RG32F, 0x17 RG32UI, 0x18 RG32I, 0x19 RGB8, 0x1A RGB8SN, 0x1B RGB8UI, 0x1C RGB8I,
  0x1D RGB16F, 0x1E RGB16, 0x1F RGB16SN, 0x20 RGB16UI, 0x21 RGB16I, 0x22 RGB32F, 0x23 RGB32UI, 0x24 RGB32I,
  0x25 RGBA8, 0x26 RGBA8SN, 0x27 RGBA8UI, 0x28 RGBA8I, 0x29 RGBA16F, 0x2A RGBA16, 0x2B RGBA16SN, 0x2C RGBA16UI,
  0x2D RGBA16I, 0x2E RGBA32F, 0x2F RGBA32UI, 0x30 RGBA32I, 0x31 STENCIL8, 0x32 DEPTH16, 0x33 DEPTH24,
  0x34 DEPTH32F, 0x35 DEPTH24_STENCIL8, 0x36 DEPTH32F_STENCIL8, 0x37 RGBX8_SRGB, 0x38 RGBA8_SRGB, 0x39 RGBA4,
  0x3A RGB5, 0x3B RGB5A1, 0x3C RGB565, 0x3D RGB10A2, 0x3E RGB10A2UI, 0x3F R11G11B10F, 0x40 RGB9E5F,
  0x41 RGB_DXT1, 0x42 RGBA_DXT1, 0x43 RGBA_DXT3, 0x44 RGBA_DXT5, ... 0x50 BPTC (BC7, learned).
- Vertex input: NVNvertexAttribState objects are 4 bytes each (BindVertexAttribState(cb, count, array)), set with
  SetFormat(format, offset) and SetStreamIndex(stream); NVNvertexStreamState: SetStride, SetDivisor. A typical
  mesh: RGB32F position at 0, RG32F at 0xC, RGBA8 at 0x24; stream stride 0x1C.- Render target formats (NVN -> hardware RT format): 0x25 -> 0xD5 (RGBA8), 0x38 -> 0xD6 (RGBA8 sRGB),
  0x3D -> 0xD1 (RGB10A2), 0x3F -> 0xE0 (R11G11B10F), 0x0A -> 0xE5 (R32F), 0x29 -> 0xCA (RGBA16F),
  depth 0x34 -> 0x0A.

Still to establish: which Maxwell constant
buffer an NVN uniform buffer index lands in (the shaders' `cbuf_mask`), where texture handles sit in the texture
constant buffer (`TextureUse::cbuf_offset` vs BindTexture index), NVN format enum to hardware render target /
vertex attribute formats, and the blend, depth and polygon enums. Instrumenting the emulated path (when it creates
a surface or input layout at a GPU address the NVN model knows, log the pair) learns these tables automatically.

## Order of work (estimates, in the same units as the recompiler work)

1. ~~Census of the functions used~~ and ~~the hook~~ (done). `recomp::Dispatch` watches the game's three NVN name
   lookup functions while entry points are resolved. `hle::nvn::Find(name)` (`src/core/hle/nvn/nvn_hle.cpp`)
   supplies host implementations, which replace the driver function in the native table, so recompiled and JIT
   callers both reach them. `MUA3_NVN_HLE=0` turns it off. The first host function is `nvnDeviceGetTextureHandle`
   (pure packing, ~3,200 calls/frame), verified in ST010.
   Single getters/setters swapped one by one give no speed-up on their own: the frame cost is in command-buffer
   encoding (sdk) plus decoding (`src/gpu`). The gain comes when a whole family (step 3) runs on host objects.
2. Device, queue, memory pool, buffer, texture, sampler objects; texture/buffer data paths. *Large.*
   **In progress (observe mode):** wrappers around Initialize/Finalize of pools, buffers and textures run the driver,
   then read each object's real properties back through the driver's getters (`CallDriver` in `nvn_hle.cpp`).
   Texture/sampler pool registrations map handle IDs to objects. ST010: 3 pools (232 MB, 1.5 GB, 40 MB),
   ~5,100 textures, ~2,100 buffers, ~28,700 texture IDs, 8 sampler IDs. Next: create the D3D11 resources for these
   objects (pool memory -> buffers/textures, reusing the format and swizzle code in `src/gpu`), still
   alongside the emulated path, and compare them with the emulated path's textures.
3. Command buffer recording and replay: state objects, draws, clears, copies, compute dispatch. *Large.*
   **Started:** `MUA3_NVN_RECORD=1` wraps Begin/EndRecording, `nvnQueueSubmitCommands` and the hot draw, bind and
   dispatch calls, building host command lists keyed by command handle (counters are logged every 600 submits).
   Finding (2026-10-24): about 3.6M commands are recorded and ~49k recordings ended, but the handles submitted
   through `nvnQueueSubmitCommands` (1.6 per frame) come from recordings with no observed commands. KTGL must
   hand its command buffers to the GPU another way. Next: find it, e.g. command memory linked by the driver
   (`AddCommandMemory`/`AddControlMemory` callbacks), or submission from a thread or path the hooks miss. Trace
   `QueueSubmitCommands` callers and the queue's control memory.
   More (same day): `nvnCommandBufferCallCommands` and `CopyCommands` are resolved by the game but never called.
   The handle submitted every frame is one constant value (0x1_0020_7716_51f9, high bits 0x1002) whose recording
   is empty. The recordings that hold the ~600 draws per frame have handles with different high bits (0x2002...).
   So KTGL submits one persistent command buffer per frame and reaches the per-frame command memory indirectly.
   Next: decode the handle bits (control vs command memory) and follow what the persistent buffer's control
   memory points at (it is in guest memory; dump it and look for the 0x2002... handles or their GPU addresses).
   **Solved:** gameplay submits make a GPU syscall that hands the rest of the call to the JIT, so the submit is now
   observed before the driver runs. With that, the host captures the full stream: per frame (one submit) about
   4,680 commands, ~668 draws (~231 indirect) and ~31 compute dispatches, with every handle accounted for. (The
   constant empty handle is a separate submit loop at boot.) Next: replay these lists on D3D11 next to the
   emulated path, translating each command with the existing renderer's state code, and compare the frames.
   Name capture also has to tolerate other functions that take "nvn..." strings: only results pointing at
   recompiled code count, a lookup function is trusted after 4 names, and host-implemented names the game never
   asks for in observed code are resolved with `ResolveMissing`.
4. Programs and resource binding (texture/sampler handles: NVN uses 64-bit texture handles from texture/sampler
   pools: `nvnTexturePoolRegisterTexture` and friends). *Medium.*
5. Window, sync, present; delete the nvdrv/GPU-emulation path once parity holds. *Medium.*
6. Then "hand-written": replace hot KTGL rendering paths (deferred lighting, post chain) with host D3D code, using
   the RT dumps as references. *Open-ended.*

Rough total for 1-5: comparable to or larger than the CPU recompiler work. Mostly debugging visual parity, with
several long test runs per family. Needs the user away from the PC for builds and soak tests.

## Notes and gotchas carried over from the emulated path

- The texture formats, block-linear swizzle, integer vertex attribute handling, depth-sampling hazards and
  resolution scaling already solved in `src/gpu` and `renderer_d3d11.cpp` carry over, now driven by NVN parameters
  instead of decoded registers.
- Compute: the game uses compute (froxel volumes, culling). NVN `nvnCommandBufferDispatchCompute` with storage
  buffers from the program's bindings; the DXBC backend stays.
