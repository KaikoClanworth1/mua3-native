#include <windows.h>
#include <Xinput.h>

#include <algorithm>
#include <cmath>

#include <SDL3/SDL.h>

#include "core/hle/hid.h"
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

// Keyboard: WASD = left stick, arrows = D-pad, numpad 8456 = right stick, L/K/I/J = A/B/X/Y,
// Q/E = L/R, 1/3 = ZL/ZR, Enter = +, Tab = -.
void ReadKeyboard(PadInput& p) {
    const bool* k = SDL_GetKeyboardState(nullptr);
    struct {
        SDL_Scancode sc;
        u64 b;
    } map[] = {
        {SDL_SCANCODE_L, A},     {SDL_SCANCODE_K, B},       {SDL_SCANCODE_I, X},          {SDL_SCANCODE_J, Y},
        {SDL_SCANCODE_Q, L},     {SDL_SCANCODE_E, R},       {SDL_SCANCODE_1, ZL},         {SDL_SCANCODE_3, ZR},
        {SDL_SCANCODE_RETURN, Plus}, {SDL_SCANCODE_TAB, Minus}, {SDL_SCANCODE_UP, Up},    {SDL_SCANCODE_DOWN, Down},
        {SDL_SCANCODE_LEFT, Left}, {SDL_SCANCODE_RIGHT, Right}, {SDL_SCANCODE_F, LStick}, {SDL_SCANCODE_KP_0, RStick},
    };
    for (auto& m : map)
        if (k[m.sc])
            p.buttons |= m.b;
    auto stick = [&](SDL_Scancode neg, SDL_Scancode pos, s32& v) {
        if (k[neg]) v = -32767;
        if (k[pos]) v = 32767;
    };
    stick(SDL_SCANCODE_A, SDL_SCANCODE_D, p.lx);
    stick(SDL_SCANCODE_S, SDL_SCANCODE_W, p.ly);
    stick(SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6, p.rx);
    stick(SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_8, p.ry);
}

} // namespace

// The skill wheel (R held) puts the skills on the face buttons by position, with no letters: with R held a face button
// goes down as the Switch button in its position, whatever the layout (#14: by letter, Xbox X fired the skill shown
// on the top circle). The choice holds until the button is released, so letting go of R mid-press changes nothing.
u64 Input::Face(int i, bool down, u64 by_layout, u64 by_pos, bool skill_modifier) {
    if (!down)
        return face_[i] = 0;
    if (!face_[i])
        face_[i] = skill_modifier ? by_pos : by_layout;
    return face_[i];
}

// An Xbox pad's state as the game's buttons and sticks.
void Input::MapXInput(const XINPUT_GAMEPAD& g, PadInput& p) {
    // By letter (default): Xbox A/B/X/Y = the game's A/B/X/Y prompts. By position: the Switch layout (Xbox A
    // is the bottom button = Switch B, and so on).
    const bool pos = button_layout_ == 1;
    const bool skills = (g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    const WORD fx[4] = {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y};
    const u64 letter[4] = {A, B, X, Y}, by_pos[4] = {B, A, Y, X};
    for (int f = 0; f < 4; f++)
        p.buttons |= Face(f, (g.wButtons & fx[f]) != 0, pos ? by_pos[f] : letter[f], by_pos[f], skills);
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

u64 Input::SimulateXInput(u16 buttons) {
    XINPUT_GAMEPAD g{};
    g.wButtons = buttons;
    PadInput p;
    p.connected = true;
    MapXInput(g, p);
    SetPad(0, p);
    return p.buttons;
}

bool Input::Init(core::InputBackend backend, int camera_speed, int button_layout) {
    backend_ = backend;
    camera_speed_ = camera_speed;
    button_layout_ = button_layout;
    LOG_INFO("Input", "Input backend: {}", backend == core::InputBackend::XInput ? "XInput" : "SDL");
    return true;
}

void Input::Update(bool focused) {
    PadInput p;
    p.connected = true;
    bool pad = false;
    if (backend_ == core::InputBackend::XInput) {
        for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
            XINPUT_STATE st{};
            if (XInputGetState(i, &st) != ERROR_SUCCESS)
                continue;
            MapXInput(st.Gamepad, p);
            pad = true;
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
            const bool skills = SDL_GetGamepadButton(pad_, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
            const SDL_GamepadButton fs[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                             SDL_GAMEPAD_BUTTON_NORTH};
            const u64 by_pos[4] = {B, A, Y, X}, xbox[4] = {A, B, X, Y};
            for (int f = 0; f < 4; f++)
                p.buttons |= Face(f, SDL_GetGamepadButton(pad_, fs[f]), face(fs[f], by_pos[f], xbox[f]), by_pos[f], skills);
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
        }
    }
    if (!pad)
        std::fill(std::begin(face_), std::end(face_), 0);
    if (focused)
        ReadKeyboard(p);
    AddStickDirections(p);
    SetPad(0, p);
}

} // namespace platform
