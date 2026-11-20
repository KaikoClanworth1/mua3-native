# Building from source

Most players don't need this: the [release download](https://github.com/KaikoClanworth1/mua3-native/releases)
builds the game for you. This page is for working on the port itself.

## How it works

The game's AArch64 code is translated to C++ and compiled for x64 ahead of time (static recompilation, about
104,000 functions, no JIT). The Switch OS services it calls are implemented natively, and its NVN graphics calls are
rendered with Direct3D 11, with the shaders translated to HLSL. No emulator runs at play time.

## Requirements

Windows 10/11 x64, Visual Studio 2026 with "Desktop development with C++", CMake 3.24+, Python 3, Git (with Git
Bash), about 30 GB free for the build and 18 GB for the game data.

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
