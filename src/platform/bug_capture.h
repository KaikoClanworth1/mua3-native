// F4 "bug capture" (debug): a dated folder with the game's latest save, a one-frame graphics capture, a screenshot,
// the last 30 s of the log and where the player is. Off unless mua3.ini has [debug] bug_capture=1.
// Format and the load route: docs/bug_capture.md.
#pragma once
#include "common/common.h"

namespace gpu {
class D3D11Renderer;
}

namespace platform {
class Display;

namespace bugcap {

void Init(bool enabled, const fs::path& save_dir);
bool Enabled();
// From any thread (F4, the test channel's "capture"); taken by the next Poll.
void Request();
// Main thread, once per UI frame: starts a requested capture.
void Poll(gpu::D3D11Renderer& renderer, Display& display);
// Inside the ImGui frame: the "Capture saved" toast.
void DrawToast();

} // namespace bugcap
} // namespace platform
