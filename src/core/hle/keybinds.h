// Keyboard and mouse bindings: which key and which mouse button press each of the game's buttons and stick directions.
// Read live by platform::Input, shown and changed on PC SETTINGS > CONTROLS > KEY BINDINGS (menu_graphics.cpp), drawn
// on the button prompts while the keyboard is in use (prompts.cpp), stored in mua3.ini's [keys] section (only the
// bindings that differ from the defaults).
#pragma once
#include <string>

#include "common/common.h"

namespace core {
struct Config;
}
namespace hle::hid {
struct PadInput;
}

namespace hle::keybinds {

enum Action : int {
    kMoveUp, kMoveDown, kMoveLeft, kMoveRight,
    kA, kB, kX, kY, kL, kR, kZL, kZR, kPlus, kMinus,
    kCamUp, kCamDown, kCamLeft, kCamRight, kLStick, kRStick,
    kDUp, kDDown, kDLeft, kDRight,
    kActions
};

// key: a USB HID keyboard usage (the same numbers as SDL scancodes), 0 = none. mouse: 1 left, 2 middle, 3 right,
// 4 back, 5 forward (SDL's button numbers), 0 = none.
struct Binding {
    u16 key = 0;
    u8 mouse = 0;
    bool operator==(const Binding&) const = default;
};

constexpr u16 kEscape = 41, kBackspace = 42, kKeyCount = 512;

Binding Get(Action a);
Binding Default(Action a);
void Set(Action a, Binding b);
void ResetAll();
// Binds `code` (a key, or a mouse button if `mouse`) to `a`. Another action that had it gets `a`'s old one (a swap);
// returns that action, or -1.
int Assign(Action a, bool mouse, u16 code);

// mua3.ini [keys]: "action=KEY[, MOUSE BUTTON]" for each binding that is not the default.
void Load(const core::Config& c);
void Store(core::Config& c);

const char* Id(Action a);            // the ini name ("a", "move_up", ...)
std::u16string Label(Action a);      // the menu's row name ("A BUTTON", "MOVE UP", ...)
std::string KeyName(u16 key);        // "L", "ENTER", "NUM 8", ... ("" for none / unknown)
std::string KeyIcon(u16 key);        // the short form a prompt icon shows ("ENT", "8", an arrow, ...), UTF-8
std::string MouseName(u8 button);    // "MOUSE LEFT", ...
u16 KeyFromName(const std::string& name);
u8 MouseFromName(const std::string& name);
// The binding as the menu shows it: "L", "L / MOUSE LEFT", "MOUSE LEFT" or "---".
std::u16string Text(Binding b);

// The game's input from the keyboard state (kKeyCount keys, by usage) and mouse buttons (bit n = button n + 1),
// OR-ed into `p` (sticks: full tilt).
void Map(const bool* keys, u32 mouse, hid::PadInput& p);

// Rebinding: the next key or mouse button pressed goes to `a` (Escape, the pad's B or 5 s with nothing pressed
// cancels, Backspace clears it).
void StartCapture(Action a);
int Capturing(); // the action being bound, or -1
// Fed every poll by platform::Input with the keyboard, mouse and pad buttons. True while the game must not see this
// input: while capturing, and after it until everything is released. A finished capture taps the game's A once
// (hid::PulseButtons) so the menu's A handler picks up the result (TakeDone).
bool Feed(const bool* keys, u32 mouse, u64 pad_buttons);
struct Outcome {
    int action = -1;
    int swapped = -1;     // the action that now has the old binding
    bool cancelled = false;
};
bool TakeDone(Outcome& out);

u32 Generation(); // changes with every binding change

} // namespace hle::keybinds
