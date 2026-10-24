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
