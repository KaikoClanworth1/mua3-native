// The FPS limit (mua3.ini [video] fps_limit): caps the game below its own frame rate.
#pragma once

namespace hle {

// Called once per game frame, after the frame is handed to the display (nvnQueuePresentTexture). With no limit, or
// one at or above the game's frame rate (its own limiter paces it then), this is a compare; otherwise it waits for
// the frame's deadline, kept on a steady grid from the previous ones.
void LimitFrameRate();

} // namespace hle
