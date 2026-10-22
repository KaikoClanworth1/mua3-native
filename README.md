# MUA3 Native

A private native Windows port of *Marvel Ultimate Alliance 3: The Black Order* (Switch), built from your own dumps.

The game's AArch64 code currently runs on the **dynarmic** JIT with high-level emulation (HLE) of the Switch OS
services. The plan is to replace that piece by piece with native code (static recompilation and native
re-implementations of hot functions, NVN on D3D11, etc.) until no emulation is left.

## Status

| Area | State |
|---|---|
| Dump install (NSP/XCI → ExeFS + RomFS, update merged via BKTR, DLC) | working |
| Boot: rtld → nnSdk → `nnMain` → title screen → title menu | working |
| Kernel HLE (threads, sync, memory, IPC/CMIF, domains) | working |
| Services: sm, fs (virtual RomFS over loose files, host saves), am, hid, nvdrv, vi, audren, time, acc, set… | working for boot |
| Input: XInput / SDL3 gamepads, keyboard | working |
| Window: SDL3 + D3D11 swapchain, ImGui overlay (F1) | working |
| GPU: GPFIFO processed for fences/semaphores/DMA; **no rendering yet** | next milestone |
| Audio: renderer protocol answered, XAudio2/SDL output ready; **no mixing yet** | pending |
| Movies (nnMedia / nvdec) | pending |

## Layout

```
Isos/      your dumps (base NSP/XCI, update, DLC)        (not in git)
keys/      prod.keys (+ title.keys) from your console     (not in git)
game/      installed data: exefs/, romfs/, control/, aoc/ (not in git)
save/      save data                                       (not in git)
src/
  loader/    NCA/PFS0/HFS0/BKTR/RomFS/NSO + installer
  crypto/    AES (CNG), key files
  core/      memory, kernel (SVCs), CPU interface, HLE services
  gpu/       GPU address space + command processor
  platform/  SDL3 window, D3D11, input, XAudio2/SDL audio
  launcher/  ImGui launcher
  tools/     nxextract (CLI installer), nsodump
```

## Build

Requirements: Visual Studio 2026 (MSVC), CMake 3.24+, Git, curl.

```bash
scripts/fetch-deps.sh
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config RelWithDebInfo
```

Outputs land in `build/bin/RelWithDebInfo/`: `MUA3Launcher.exe`, `mua3.exe`, `nxextract.exe`, `nsodump.exe`.

## Run

1. Put `prod.keys` in `keys/` and the dumps in `Isos/`.
2. Start `MUA3Launcher.exe` → **Install** (≈17 GB is extracted to `game/`) → **Play**.

Settings are stored in `mua3.ini` in the project folder. Runtime flags for development:
`mua3.exe --debug | --trace | --headless | --watch N (thread dumps) | --autopress N (tap A every 3 s)`.
Logs go to `mua3.log`.

Keyboard: WASD move, L/K/I/J = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter = +, Tab = −, F11 fullscreen, F1 overlay.
