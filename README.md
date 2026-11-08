# MUA3 Native

A native Windows port of *Marvel Ultimate Alliance 3: The Black Order* (Nintendo Switch), made by **static
recompilation**: the game's AArch64 code is translated to C++ and compiled for x64 ahead of time, the Switch OS
services it calls are implemented natively, and its NVN graphics calls are rendered with Direct3D 11. No emulator
runs at play time.

**This repository contains no game code, data or keys.** You need your own copy of the game, dumped from your own
Switch along with that console's keys. The recompiled game is built on your PC from your dump; it is never
distributed here.

## Status

| Area | State |
|---|---|
| Installer: NSP/XCI base game, update (BKTR merge) and DLC, from the launcher | working |
| Game code: statically recompiled (104,176 functions, 99.998% of the code), no JIT | working |
| OS services: kernel, IPC, fs (RomFS over loose files, saves), hid, am, nvdrv, vi, audren, time, set, ... | working |
| Rendering: NVN command stream replayed on D3D11, shaders translated to HLSL, cached | working, some effects missing |
| Resolution: native 1080p, 1440p, 4K (internal resolution scales with the display) | working |
| Frame rate: 30 (original), 60, or higher (experimental, patched timing) | working |
| Audio (XAudio2 or SDL3), movies (Media Foundation), XInput / SDL3 gamepads, keyboard | working |

The story has been played through several chapters. Known issues: occasional missing or black UI/effect panels,
the frame rate is limited by the render thread in large fights, a few shaders fail to translate.

## Playing (release download)

1. Download `MUA3Native-v…-win64.zip` from [Releases](https://github.com/KaikoClanworth1/mua3-native/releases) and
   unzip it anywhere (about 25 GB free is needed for the game and the build).
2. Run `MUA3Launcher.exe` → **Install**: choose your `prod.keys`, add the base game, the 4.0.1 update and any DLC
   (`.nsp` / `.xci`), install.
3. **Step 5, Build the game**: if asked, install Microsoft's free
   [Build Tools for Visual Studio 2026](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio)
   with "Desktop development with C++". Then press **Build the game**: the launcher unpacks the game's code from
   your install, translates it to C++ and compiles your own `mua3.exe` (about an hour on 8 cores, ~5 GB).
4. **Play**.

The release contains only this project's code (the launcher, the recompiler and the port's libraries); the game is
built from your copy on your PC.

## Building from source

Requirements: Windows 10/11 x64, Visual Studio 2026 with "Desktop development with C++", CMake 3.24+,
Python 3, Git (with Git Bash), about 30 GB free for the build and 18 GB for the game data.

```bash
scripts/fetch-deps.sh                                   # dynarmic, SDL3, Dear ImGui, Boost headers into external/
cmake -S . -B build -G "Visual Studio 18 2026" -A x64   # or "Visual Studio 17 2022"
cmake --build build --config RelWithDebInfo --target MUA3Launcher nsodump recomp
```

1. **Install the game data.** Run `build\bin\RelWithDebInfo\MUA3Launcher.exe` → **Install**: choose your
   `prod.keys`, add the base game, the 4.0.1 update and any DLC (`.nsp` / `.xci`), choose a folder, install.
2. **Recompile the game code** from your installed copy (the long step: about an hour on 8 threads):
   ```bash
   build\bin\RelWithDebInfo\nsodump.exe game\exefs build\recomp_work\nso
   python scripts\recomp_libbuild.py 8        # number of parallel compiler jobs
   ```
   This generates C++ from your NSOs into `build\recomp_full` and compiles `build\recomp_full\nxrecomp_full.lib`.
3. **Build the game:** `cmake --build build --config RelWithDebInfo --target mua3`
4. Copy `mua3.exe` next to the launcher (or run the launcher from the build folder) and press **Play**.

Optional: `scripts\make_icon.ps1` makes the executables' icon from your installed game (rebuild afterwards).

## Using it

Settings live in `mua3.ini` and are edited in the launcher: presets (Recommended = the original 30 fps, Smooth = 60,
High refresh), resolution, camera speed, language, audio. Keys: F11 fullscreen, F12 screenshot, F1 debug overlay.
Keyboard: WASD move, L/K/I/J = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter = +, Tab = −. Logs go to `mua3.log`.

## Layout

```
src/
  loader/    NCA / PFS0 / HFS0 / BKTR / RomFS / NSO, the installer
  crypto/    AES (CNG), key files
  core/      memory, kernel (SVCs), CPU runtime for the recompiled code, HLE services, NVN layer
  gpu/       D3D11 renderer, shader translator (Maxwell -> HLSL / DXBC), texture and shader caches
  platform/  SDL3 window, D3D11 swap chain, input, XAudio2 / SDL audio
  launcher/  the ImGui launcher (installer, settings)
  tools/     recomp (AArch64 -> C++), nsodump, nxextract (CLI installer), threadprof, shadertool, ...
scripts/     fetch-deps.sh, recomp_libbuild.py, make_release.ps1 (the release package), bench.ps1, make_icon.ps1
```

## Legal

*Marvel Ultimate Alliance 3: The Black Order* is © MARVEL, © Nintendo, developed by KOEI TECMO GAMES. This is an
unofficial fan project, not affiliated with or endorsed by them. It contains no copyrighted game material; use it
only with a game you own, dumped from your own console.

Third-party code: [dynarmic](https://github.com/azahar-emu/dynarmic) (0BSD; its A64 decoder and IR are used by the
recompiler), [SDL3](https://github.com/libsdl-org/SDL) (zlib), [Dear ImGui](https://github.com/ocornut/imgui) (MIT),
[Boost](https://www.boost.org) (BSL-1.0).
