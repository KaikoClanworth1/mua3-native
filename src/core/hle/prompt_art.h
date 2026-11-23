// Button prompt art for the devices the game has no icons for: keyboard keycaps, the mouse, Xbox and PlayStation
// pads. Original artwork: every icon is drawn by scripts/prompt_art/gen.py from its own shapes and its own alphabet
// (no font, no vendor artwork, no game data) in the style of the game's prompts, and embedded packed in
// prompt_art_data.cpp. To change an icon, edit the script and run it again.
//
// Every icon is 64 x 64 RGBA8 with straight alpha (the size of the game's icons, drawn on the same centres), ready
// for menu::EncodeBC3Rgba after a conversion to float. An empty vector means "no art for this": keep the fallback.
#pragma once
#include <string_view>
#include <vector>

#include "common/common.h"

namespace hle::prompt_art {

constexpr int kIconSize = 64;
using Rgba = std::vector<u8>; // kIconSize * kIconSize * 4

// The keycap of a key by its USB HID usage (keybinds::Binding::key). Right-hand Ctrl / Shift / Alt carry a small R,
// number pad keys a small N. hold: the symbol in orange, as the game marks a button to keep held.
Rgba Key(u16 hid_usage, bool hold = false);

// The mouse with a button lit: 1 left, 2 middle, 3 right, 4 back, 5 forward (keybinds::Binding::mouse),
// 6 wheel up, 7 wheel down, 0 the whole mouse with arrows ("move the mouse").
Rgba Mouse(u8 button, bool hold = false);

// Four small keycaps in an inverted T (W over A S D, the arrow keys, any four keys) for a prompt that shows a stick.
// Empty if one of the keys has no small cap (only one-character keys and the arrows have one).
Rgba Cluster(u16 up, u16 left, u16 down, u16 right, bool hold = false);

enum class Pad { Xbox, PlayStation };

// The pad's icon for one of the GAME's icons, by the game's name ("btn_A_icon", "btn_A_icon_hold", "btn_ZL_icon",
// "btn_Plus_JOY_icon", "stick_btn_L_push_icon", "stick_L_roll_R_icon", "btn_Cross_UPDOWN_icon",
// "btn_dir_all_ABXY_icon", ...). layout: 0 = the button in the same place as on an Xbox pad with that letter
// (the game's A is the bottom button: Xbox A, PlayStation cross), 1 = Switch positions (the game's A is the right
// button: Xbox B, PlayStation circle). Empty for the icons that stay the game's own (HOME, Capture, SL, SR, the
// one-button-lit diamonds, the arrow discs).
Rgba PadIcon(Pad pad, std::string_view game_icon, int layout = 0);

// An icon by this set's own name ("xbox/a", "ps/cross_hold", "pad/ls_push", ...; the list is kNamed in
// prompt_art_data.cpp). Empty if there is none.
Rgba Named(std::string_view name);

} // namespace hle::prompt_art
