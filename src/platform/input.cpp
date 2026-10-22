#include <windows.h>
#include <Xinput.h>

#include <SDL3/SDL.h>

#include "core/hle/hid.h"
#include "platform/platform.h"

#pragma comment(lib, "xinput.lib")

namespace platform {

using namespace hle::hid;

namespace {

s32 Axis(int v, int deadzone) {
    if (std::abs(v) < deadzone)
        return 0;
    return std::clamp(v, -32767, 32767);
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

bool Input::Init(core::InputBackend backend) {
    backend_ = backend;
    LOG_INFO("Input", "Input backend: {}", backend == core::InputBackend::XInput ? "XInput" : "SDL");
    return true;
}

void Input::Update(bool focused) {
    PadInput p;
    p.connected = true;
    if (backend_ == core::InputBackend::XInput) {
        for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
            XINPUT_STATE st{};
            if (XInputGetState(i, &st) != ERROR_SUCCESS)
                continue;
            const auto& g = st.Gamepad;
            struct {
                WORD x;
                u64 b;
            } map[] = {
                {XINPUT_GAMEPAD_A, A}, {XINPUT_GAMEPAD_B, B}, {XINPUT_GAMEPAD_X, Y}, {XINPUT_GAMEPAD_Y, X},
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
            p.lx = Axis(g.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
            p.ly = Axis(g.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
            p.rx = Axis(g.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
            p.ry = Axis(g.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
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
            // SDL reports buttons by position; map positions to the Switch layout.
            struct {
                SDL_GamepadButton s;
                u64 b;
            } map[] = {
                {SDL_GAMEPAD_BUTTON_EAST, A}, {SDL_GAMEPAD_BUTTON_SOUTH, B}, {SDL_GAMEPAD_BUTTON_NORTH, X},
                {SDL_GAMEPAD_BUTTON_WEST, Y}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, L},
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
            p.lx = Axis(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTX), 7000);
            p.ly = -Axis(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTY), 7000);
            p.rx = Axis(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHTX), 7000);
            p.ry = -Axis(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHTY), 7000);
        }
    }
    if (focused)
        ReadKeyboard(p);
    AddStickDirections(p);
    SetPad(0, p);
}

} // namespace platform
