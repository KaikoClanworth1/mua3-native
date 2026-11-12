# F4 bug capture

A debug key for players helping with testing: press **F4** at a broken moment and the game writes a folder that
the developers can load and inspect later. It is not an emulator save state: the game's code runs natively, so its
threads, stacks and the Direct3D state can't be saved and restored. The capture holds the nearest game save
instead, plus everything needed to look at that frame offline.

## Turning it on

The key is off unless `mua3.ini` (in the game folder) has:

```ini
[debug]
bug_capture=1
```

When the setting is off, nothing happens and nothing is done each frame. When it is on, the renderer checks one
pointer per draw. The game keeps the section when it rewrites the ini (for example after a settings change).
Test instances can also take a capture through the command channel: `inst.ps1 cmd <i> capture`.

When the folder is written, a "Capture saved" message shows at the top of the screen for a few seconds.

## What it writes: `bug_captures\YYYY-MM-DD_HHMMSS\`

| File | Contents |
|---|---|
| `info.txt` | Build version, time, stage (for example `ST020`) and stage file, the last scenario file, the characters loaded this run (party, bosses, summons), the save's chapter and date, key settings, and the frame-capture summary |
| `save\` | Copies of the game's save files (`save00`, `savedatainfo`), read from the save folder. The user's save is never written to |
| `screenshot.bmp` | The captured game frame, at render size |
| `window.bmp` | The window as shown, overlays included |
| `log_last_30s.txt` | The last 30 seconds of `mua3.log` |
| `frame\` | The graphics capture (see below) |

### The save

The game is not asked to save. Calling its save routine from outside its own flow (for example during the Sandman
fight, where it refuses to save) could corrupt the user's real save, and that save is the folder the game writes to.
So the capture copies the latest save the game wrote itself: the last checkpoint or autosave before the capture.
`info.txt` gives the stage the player was in at capture time, which can be later than the save.

The game keeps checkpoint progress inside a chapter in memory. Its autosaves during a chapter rewrite `save00` with
the same chapter data; only the date in `savedatainfo` changes. For example, a capture in the Sandman fight
(ST020_06) had a `save00` byte-identical to one written at the start of the Raft (ST020_00). So a capture's save
resumes at the chapter start or the last hub, not at the moment of the capture.

**Loading it** (test instances, under the test lock):

```
powershell -File build\testinst\inst.ps1 tocapture <i> "<path>\bug_captures\<folder>"
```

This copies the capture's `save\` into the test instance, boots it, chooses CONTINUE on the title screen, waits for
a stage to load, and then waits for gameplay. The script prints which stage it reached.

### The graphics capture (`frame\`)

It records one whole frame: every draw, compute dispatch and clear from one present to the next, in submission
order. Before it starts, the capture checks the drive. It keeps 2 GB free for the game plus 128 MB for the rest of the
folder, and shrinks the budget to fit. When less than 128 MB would be left for the frame, it writes nothing and says
so on screen. The total size is capped at 2.5 GB: 1 GB for textures, 1 GB for buffers and 512 MB for target images. Buffer
contents are deduplicated by hash before they count, so constants bound by many draws are stored and counted once.
Anything over a cap is still listed in `events.jsonl`, but without a file, and the summary gives the size skipped.

- `events.jsonl` has one JSON object per line:
  - **draw**:
    - `state` is the index into `states.bin`.
    - It has topology, first/count, indirect, the five program addresses, and the colour and depth targets as
      `[slot, addr, format, w, h]`.
    - `cb` lists the constant buffers as `[stage, slot, addr, size, file]`, `vb` the vertex streams as
      `[stream, addr, size, stride, file]`, and `ib` the index buffer as `[addr, format, file]`.
    - `tex` lists each texture with stage, slot, handle, address, format, type, size, the 8 TIC words, and its file.
    - Draws the renderer submitted also have `drawn: true` and the `vs`/`ps` shader hashes (the `shader_dump` /
      shader pack hash). Draws without `drawn` were skipped by the renderer.
  - **dispatch**: program, offset, grid, block, shared memory size, constant buffers, and the texture and sampler
    pool addresses. Dispatches the renderer ran also have `cs`, the host compute shader hash (its guest program is in
    `shaders\<cs>.cs.bin`), and `gmem` as `[window, addr, size, before file, after file]`.
  - **clear**: the colour target and colour, or the depth and stencil values.
  - **present**: the presented surface.
  - Draws (those drawn), dispatches and clears end with `gpu_us`: the GPU time of that event's own D3D11 call in
    microseconds, from a timestamp pair right around it (copies and uploads the renderer issues before the call are
    not included). The captured frame itself runs slowly (the capture writes its files), so only these per-call
    times are meaningful, not the frame's length.
- `gpu_top.txt`: the 50 events with the highest `gpu_us`, with the frame's total.
- `gpu_passes.txt`: GPU time per pass over the 30 normal frames after the capture: a pass is a run of draws into the
  same render targets, or a run of dispatches of one compute program. It includes everything the GPU did in that
  span (buffer uploads, copy-backs, copies), so a pass far above its events' `gpu_us` is spending its time on
  transfers.
- `states.bin`: each draw's `gpu::DrawState` (`src/gpu/draw_state.h`), back to back. `README.txt` gives the struct
  size for that build. A replayer built from the same source can `memcpy` them straight back.
- `buf\`: deduplicated by content hash:
  - guest memory: constant buffers (`cb_`, up to 64 KB each), vertex streams (`vb_`, up to 64 MB each), index
    ranges (`ib_`) and texture buffers (`tb_`: TIC type 6, listed with the textures);
  - compute global memory windows (`gmem_`): each window's contents on the host GPU before and after the dispatch.
- `tex\`: each texture as the frame first sampled it: DDS files with a DX10 header in the host DXGI format (2D,
  arrays, cube maps and 3D textures, whose names end in `WxHxD`).
  Render targets read as textures are included, with `_rt` in the name.
- `rt\`: the final contents of every colour target the frame wrote, as BMPs.
- `shaders\`: the guest Maxwell programs the frame used, `<hash>.vs.bin` / `<hash>.ps.bin`. They are copied from
  `shader_dump`, so `shadertool` reads them the same way.

## Code

- `src/platform/bug_capture.cpp`: the key, the folder, the save copy, the log and location, and the message.
- `src/gpu/d3d11/frame_capture.cpp`: the frame recorder. The renderer calls it from `Draw`, `Dispatch`,
  `ClearColor`, `ClearDepthStencil` and `Present`, only while `capture_` is set.
