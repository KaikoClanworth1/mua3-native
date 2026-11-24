#include <windows.h>
#include <Xinput.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>

#include <SDL3/SDL.h>

#include "core/hle/camera_speed.h"
#include "core/hle/hid.h"
#include "core/hle/keybinds.h"
#include "core/hle/prompts.h"
#include "platform/platform.h"

#pragma comment(lib, "xinput.lib")

namespace platform {

using namespace hle::hid;

namespace {

// A stick with a radial deadzone, rescaled so the travel past the deadzone covers the whole range. (Clipping each
// axis at the deadzone made the game see ~27% deflection the moment a stick left it: the camera started fast, with
// no fine control.) The right stick is not scaled: CAMERA SPEED scales the game's turn speed (hle::camera_speed).
void Stick(int x, int y, int deadzone, double scale, s32& ox, s32& oy) {
    const double m = std::sqrt(double(x) * x + double(y) * y);
    if (m <= deadzone) {
        ox = oy = 0;
        return;
    }
    const double mag = std::min(1.0, (m - deadzone) / (32767.0 - deadzone)) * scale;
    ox = s32(std::clamp(x / m * mag * 32767.0, -32767.0, 32767.0));
    oy = s32(std::clamp(y / m * mag * 32767.0, -32767.0, 32767.0));
}

void AddStickDirections(PadInput& p) {
    constexpr s32 t = 16384;
    if (p.lx < -t) p.buttons |= LStickLeft;
    if (p.lx > t) p.buttons |= LStickRight;
    if (p.ly > t) p.buttons |= LStickUp;
    if (p.ly < -t) p.buttons |= LStickDown;
    if (p.rx < -t) p.buttons |= RStickLeft;
    if (p.rx > t) p.buttons |= RStickRight;
    if (p.ry > t) p.buttons |= RStickUp;
    if (p.ry < -t) p.buttons |= RStickDown;
}

} // namespace

// The wheels the game opens while a button is held show their choices on the face buttons by position, with no
// letters: R the skills, ZR the team-up skills, ZL the emotes (L has none: it guards). While one of those is held a
// face button goes down as the Switch button in its position, whatever the layout (#14: by letter, Xbox X fired the
// top choice instead of the left one). The choice holds until the button is released, so letting go of the trigger
// mid-press changes nothing.
u64 Input::Face(int i, bool down, u64 by_layout, u64 by_pos, bool wheel) {
    if (!down)
        return face_[i] = 0;
    if (!face_[i])
        face_[i] = wheel ? by_pos : by_layout;
    return face_[i];
}

// An Xbox pad's state as the game's buttons and sticks.
void Input::MapXInput(const XINPUT_GAMEPAD& g, PadInput& p) {
    // By letter (default): Xbox A/B/X/Y = the game's A/B/X/Y prompts. By position: the Switch layout (Xbox A
    // is the bottom button = Switch B, and so on).
    const bool pos = button_layout_ == 1;
    const bool wheel = (g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) || g.bRightTrigger > 64 || g.bLeftTrigger > 64;
    const WORD fx[4] = {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y};
    const u64 letter[4] = {A, B, X, Y}, by_pos[4] = {B, A, Y, X};
    for (int f = 0; f < 4; f++)
        p.buttons |= Face(f, (g.wButtons & fx[f]) != 0, pos ? by_pos[f] : letter[f], by_pos[f], wheel);
    struct {
        WORD x;
        u64 b;
    } map[] = {
        {XINPUT_GAMEPAD_LEFT_SHOULDER, L}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, R},
        {XINPUT_GAMEPAD_START, Plus}, {XINPUT_GAMEPAD_BACK, Minus},
        {XINPUT_GAMEPAD_LEFT_THUMB, LStick}, {XINPUT_GAMEPAD_RIGHT_THUMB, RStick},
        {XINPUT_GAMEPAD_DPAD_UP, Up}, {XINPUT_GAMEPAD_DPAD_DOWN, Down},
        {XINPUT_GAMEPAD_DPAD_LEFT, Left}, {XINPUT_GAMEPAD_DPAD_RIGHT, Right},
    };
    for (auto& m : map)
        if (g.wButtons & m.x)
            p.buttons |= m.b;
    if (g.bLeftTrigger > 64) p.buttons |= ZL;
    if (g.bRightTrigger > 64) p.buttons |= ZR;
    // (XInput's suggested deadzones are 24% / 27%: far too much on top of the game's own, #17)
    Stick(g.sThumbLX, g.sThumbLY, left_dz_ * 32767 / 100, 1.0, p.lx, p.ly);
    Stick(g.sThumbRX, g.sThumbRY, right_dz_ * 32767 / 100, 1.0, p.rx, p.ry);
}

u64 Input::SimulateXInput(u32 buttons) {
    XINPUT_GAMEPAD g{};
    g.wButtons = WORD(buttons);
    g.bLeftTrigger = buttons & kSimLeftTrigger ? 255 : 0;
    g.bRightTrigger = buttons & kSimRightTrigger ? 255 : 0;
    PadInput p;
    p.connected = true;
    MapXInput(g, p);
    SetPad(0, p);
    hle::prompts::SetDevice(hle::prompts::Device::Xbox);
    return p.buttons;
}

void Input::SetButtonLayout(int layout) {
    button_layout_ = layout;
    hle::prompts::SetLayout(layout);
}

bool Input::Init(core::InputBackend backend, int camera_speed, int button_layout) {
    backend_ = backend;
    camera_speed_ = camera_speed;
    button_layout_ = button_layout;
    hle::prompts::SetLayout(button_layout);
    LOG_INFO("Input", "Input backend: {}", backend == core::InputBackend::XInput ? "XInput" : "SDL");
    return true;
}

void Input::Update(bool focused) {
    PadInput p;
    p.connected = true;
    bool pad = false;
    bool act[4] = {}; // by hle::prompts::Device: the player is using it this poll
    if (backend_ == core::InputBackend::XInput) {
        for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
            XINPUT_STATE st{};
            if (XInputGetState(i, &st) != ERROR_SUCCESS)
                continue;
            MapXInput(st.Gamepad, p);
            pad = true;
            const auto& g = st.Gamepad;
            const bool moved = std::abs(g.sThumbLX) > 12000 || std::abs(g.sThumbLY) > 12000 ||
                               std::abs(g.sThumbRX) > 12000 || std::abs(g.sThumbRY) > 12000;
            if (g.wButtons || g.bLeftTrigger > 64 || g.bRightTrigger > 64 || moved || !pad_seen_)
                act[int(hle::prompts::Device::Xbox)] = true;
            device_name_ = fmt::format("XInput controller {}", i + 1);
            break;
        }
    } else {
        if (!pad_) {
            int n = 0;
            SDL_JoystickID* ids = SDL_GetGamepads(&n);
            if (n > 0) {
                pad_ = SDL_OpenGamepad(ids[0]);
                if (pad_)
                    device_name_ = SDL_GetGamepadName(pad_) ? SDL_GetGamepadName(pad_) : "SDL gamepad";
            }
            SDL_free(ids);
        }
        if (pad_ && !SDL_GamepadConnected(pad_)) {
            SDL_CloseGamepad(pad_);
            pad_ = nullptr;
            device_name_ = "Keyboard";
        }
        if (pad_) {
            // SDL reports buttons by position. By position: the Switch layout. By letter (default): each face button's
            // printed label (a Switch Pro pad ends up the same; PlayStation: cross = A, circle = B, square = X,
            // triangle = Y; unknown labels count as an Xbox pad).
            auto face = [&](SDL_GamepadButton s, u64 by_pos, u64 xbox) -> u64 {
                if (button_layout_ == 1)
                    return by_pos;
                switch (SDL_GetGamepadButtonLabel(pad_, s)) {
                case SDL_GAMEPAD_BUTTON_LABEL_A: case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return A;
                case SDL_GAMEPAD_BUTTON_LABEL_B: case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return B;
                case SDL_GAMEPAD_BUTTON_LABEL_X: case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return X;
                case SDL_GAMEPAD_BUTTON_LABEL_Y: case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return Y;
                default: return xbox;
                }
            };
            const bool wheel = SDL_GetGamepadButton(pad_, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ||
                               SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000 ||
                               SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000;
            const SDL_GamepadButton fs[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                             SDL_GAMEPAD_BUTTON_NORTH};
            const u64 by_pos[4] = {B, A, Y, X}, xbox[4] = {A, B, X, Y};
            for (int f = 0; f < 4; f++)
                p.buttons |= Face(f, SDL_GetGamepadButton(pad_, fs[f]), face(fs[f], by_pos[f], xbox[f]), by_pos[f], wheel);
            pad = true;
            struct {
                SDL_GamepadButton s;
                u64 b;
            } map[] = {
                {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, L},
                {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, R}, {SDL_GAMEPAD_BUTTON_START, Plus},
                {SDL_GAMEPAD_BUTTON_BACK, Minus}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, LStick},
                {SDL_GAMEPAD_BUTTON_RIGHT_STICK, RStick}, {SDL_GAMEPAD_BUTTON_DPAD_UP, Up},
                {SDL_GAMEPAD_BUTTON_DPAD_DOWN, Down}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, Left},
                {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Right},
            };
            for (auto& m : map)
                if (SDL_GetGamepadButton(pad_, m.s))
                    p.buttons |= m.b;
            if (SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000) p.buttons |= ZL;
            if (SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000) p.buttons |= ZR;
            Stick(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTX), -SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTY), left_dz_ * 32767 / 100,
                  1.0, p.lx, p.ly);
            Stick(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHTX), -SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHTY), right_dz_ * 32767 / 100,
                  1.0, p.rx, p.ry);
            const bool moved = std::abs(p.lx) > 8000 || std::abs(p.ly) > 8000 || std::abs(p.rx) > 8000 || std::abs(p.ry) > 8000;
            if (p.buttons || moved || !pad_seen_) {
                switch (SDL_GetGamepadType(pad_)) {
                case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5:
                    act[int(hle::prompts::Device::PlayStation)] = true;
                    break;
                case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
                case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
                    act[int(hle::prompts::Device::Switch)] = true;
                    break;
                default:
                    act[int(hle::prompts::Device::Xbox)] = true;
                }
            }
        }
    }
    if (!pad)
        std::fill(std::begin(face_), std::end(face_), 0);
    pad_seen_ = pad;
    bool keys[hle::keybinds::kKeyCount] = {};
    u32 mouse = 0;
    if (focused) {
        int n = 0;
        const bool* k = SDL_GetKeyboardState(&n);
        std::copy(k, k + std::min<int>(n, hle::keybinds::kKeyCount), keys);
        mouse = SDL_GetMouseState(nullptr, nullptr);
    }
    if (KeyboardAndMouse(keys, mouse, p))
        act[int(hle::prompts::Device::Keyboard)] = true;
    // The prompts follow a new touch only (a device's input appearing, not held or drifting), at most once a second,
    // so a drifting stick or a held key cannot make them flicker between two devices.
    std::optional<hle::prompts::Device> device;
    for (int d = 0; d < 4; d++)
        if (act[d] && !active_[d])
            device = hle::prompts::Device(d);
    std::copy(std::begin(act), std::end(act), active_);
    const auto now = std::chrono::steady_clock::now();
    if (device && *device != hle::prompts::CurrentDevice() && now - device_changed_ >= std::chrono::seconds(1)) {
        hle::prompts::SetDevice(*device);
        device_changed_ = now;
    }
    AddStickDirections(p);
    SetPad(0, p);
}

// The keyboard and mouse through the bindings (hle::keybinds), on top of the pad's input in `p`; while a binding is
// being changed the game sees nothing. True if a key or mouse button the bindings use is down.
bool Input::KeyboardAndMouse(const bool* keys, u32 mouse, PadInput& p) {
    if (hle::keybinds::Feed(keys, mouse, p.buttons)) {
        p = PadInput{};
        p.connected = true;
        return false;
    }
    PadInput k;
    hle::keybinds::Map(keys, mouse, k);
    p.buttons |= k.buttons;
    if (k.lx) p.lx = k.lx;
    if (k.ly) p.ly = k.ly;
    if (k.rx) p.rx = k.rx;
    if (k.ry) p.ry = k.ry;
    return k.buttons || k.lx || k.ly || k.rx || k.ry;
}

void Input::SetMouseCamera(int mode, int sensitivity) {
    mouse_camera_ = mode != 0;
    hle::camera_speed::SetMouse(mode != 0, sensitivity, mode == 2);
}

// MOUSE CAMERA: the mouse is captured (relative mode, no cursor) only while the keyboard and mouse are the device in
// use (a pad player keeps a free cursor), the player camera runs and the window has the focus; its motion goes to the
// camera's yaw and pitch (hle::camera_speed). Menus, pause, cut-scenes, another window, a pad and KEY BINDINGS'
// capture release it. MUA3_MOUSE_CAMERA_TEST=1 (test aid): the window counts as focused and SDL is not asked (hidden
// test instances; the `mouse` command feeds counts).
void Input::MouseCamera(SDL_Window* window, bool focused) {
    static const bool test = std::getenv("MUA3_MOUSE_CAMERA_TEST") != nullptr;
    const char* why = !mouse_camera_                          ? "MOUSE CAMERA off"
                      : !(focused || test)                    ? "window not focused"
                      : hle::prompts::CurrentDevice() != hle::prompts::Device::Keyboard
                                                              ? "a pad is in use"
                      : hle::keybinds::Capturing() >= 0       ? "binding a key"
                      : !hle::camera_speed::CameraActive()    ? "camera not running: menu, pause or cut-scene"
                                                              : nullptr;
    const bool want = why == nullptr;
    if (want != mouse_captured_) {
        mouse_captured_ = want;
        if (!test && window) {
            SDL_SetWindowRelativeMouseMode(window, want);
            float x, y;
            SDL_GetRelativeMouseState(&x, &y); // (motion from before the change is dropped)
        }
        LOG_INFO("Input", "mouse camera: {}", want ? std::string("captured") : fmt::format("released ({})", why));
    }
    if (mouse_captured_ && !test) {
        float dx = 0, dy = 0;
        SDL_GetRelativeMouseState(&dx, &dy);
        if (dx != 0 || dy != 0)
            hle::camera_speed::AddMouse(dx, dy);
    }
}

u64 Input::SimulateKeys(const std::vector<u16>& keys, u32 mouse) {
    bool k[hle::keybinds::kKeyCount] = {};
    for (u16 c : keys)
        if (c < hle::keybinds::kKeyCount)
            k[c] = true;
    PadInput p;
    p.connected = true;
    if (KeyboardAndMouse(k, mouse, p))
        hle::prompts::SetDevice(hle::prompts::Device::Keyboard);
    AddStickDirections(p);
    SetPad(0, p);
    return p.buttons;
}

} // namespace platform
