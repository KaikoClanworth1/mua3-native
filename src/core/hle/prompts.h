// Button prompt icons that follow the controller in use (#14 follow-up): the game draws its prompts from the icons of
// UI/screenlayout/80_button_icon (Switch buttons). Those textures are recognised as the renderer decodes them and
// replaced with icons drawn here in the same style (Xbox letters by default), and rewritten in place when the player
// switches devices or BUTTONS layouts.
#pragma once
#include "common/common.h"

namespace hle::prompts {

enum class Device { Switch, Xbox, PlayStation, Keyboard };

// Reads the game's icon pack (as served, mods included), draws the replacement sets and hooks the texture cache.
void Init();

// The device whose buttons the prompts show, and core::Config::button_layout (0 by letter, 1 Switch positions). Live.
void SetDevice(Device d);
void SetLayout(int layout);
// The key bindings changed (hle::keybinds): the keyboard icons are drawn again.
void KeysChanged();
Device CurrentDevice();
const char* DeviceName(Device d);

} // namespace hle::prompts
