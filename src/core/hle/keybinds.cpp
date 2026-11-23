#include "core/hle/keybinds.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <mutex>

#include "core/config.h"
#include "core/hle/hid.h"

namespace hle::keybinds {
namespace {

struct Info {
    const char* id;
    const char16_t* label;
    u64 button;  // a game button, or 0 for a stick direction:
    int stick;   // 0 left, 1 right
    int axis;    // 0 x, 1 y
    int sign;    // -1 / +1 (+y = up)
    Binding def; // the default
};
// The defaults are the keys the port always had: WASD moves, L / K / I / J are A / B / X / Y, the numpad turns the camera.
const Info kInfo[kActions] = {
    {"move_up", u"MOVE UP", 0, 0, 1, 1, {26}},
    {"move_down", u"MOVE DOWN", 0, 0, 1, -1, {22}},
    {"move_left", u"MOVE LEFT", 0, 0, 0, -1, {4}},
    {"move_right", u"MOVE RIGHT", 0, 0, 0, 1, {7}},
    {"a", u"A BUTTON", hid::A, 0, 0, 0, {15}},
    {"b", u"B BUTTON", hid::B, 0, 0, 0, {14}},
    {"x", u"X BUTTON", hid::X, 0, 0, 0, {12}},
    {"y", u"Y BUTTON", hid::Y, 0, 0, 0, {13}},
    {"l", u"L BUTTON", hid::L, 0, 0, 0, {20}},
    {"r", u"R BUTTON", hid::R, 0, 0, 0, {8}},
    {"zl", u"ZL BUTTON", hid::ZL, 0, 0, 0, {30}},
    {"zr", u"ZR BUTTON", hid::ZR, 0, 0, 0, {32}},
    {"plus", u"+ BUTTON (MENU)", hid::Plus, 0, 0, 0, {40}},
    {"minus", u"- BUTTON", hid::Minus, 0, 0, 0, {43}},
    {"camera_up", u"CAMERA UP", 0, 1, 1, 1, {96}},
    {"camera_down", u"CAMERA DOWN", 0, 1, 1, -1, {93}},
    {"camera_left", u"CAMERA LEFT", 0, 1, 0, -1, {92}},
    {"camera_right", u"CAMERA RIGHT", 0, 1, 0, 1, {94}},
    {"left_stick", u"LEFT STICK PRESS", hid::LStick, 0, 0, 0, {9}},
    {"right_stick", u"RIGHT STICK PRESS", hid::RStick, 0, 0, 0, {98}},
    {"dpad_up", u"D-PAD UP", hid::Up, 0, 0, 0, {82}},
    {"dpad_down", u"D-PAD DOWN", hid::Down, 0, 0, 0, {81}},
    {"dpad_left", u"D-PAD LEFT", hid::Left, 0, 0, 0, {80}},
    {"dpad_right", u"D-PAD RIGHT", hid::Right, 0, 0, 0, {79}},
};

struct KeyNameEntry {
    u16 key;
    const char* name; // menu and mua3.ini
    const char* icon; // on a prompt icon (UTF-8)
};
const KeyNameEntry kNames[] = {
    {40, "ENTER", "ENTER"}, {41, "ESC", "ESC"}, {42, "BACKSPACE", "BKSP"}, {43, "TAB", "TAB"}, {44, "SPACE", "SPACE"},
    {45, "MINUS", "-"}, {46, "EQUALS", "="}, {47, "LEFT BRACKET", "["}, {48, "RIGHT BRACKET", "]"},
    {49, "BACKSLASH", "\\"}, {51, "SEMICOLON", ";"}, {52, "APOSTROPHE", "'"}, {53, "GRAVE", "`"},
    {54, "COMMA", ","}, {55, "PERIOD", "."}, {56, "SLASH", "/"}, {57, "CAPS LOCK", "CAPS"},
    {70, "PRINT SCREEN", "PRTSC"}, {71, "SCROLL LOCK", "SCRLK"}, {72, "PAUSE", "PAUSE"}, {73, "INSERT", "INS"},
    {74, "HOME", "HOME"}, {75, "PAGE UP", "PGUP"}, {76, "DELETE", "DEL"}, {77, "END", "END"},
    {78, "PAGE DOWN", "PGDN"}, {79, "RIGHT ARROW", "\xE2\x86\x92"}, {80, "LEFT ARROW", "\xE2\x86\x90"},
    {81, "DOWN ARROW", "\xE2\x86\x93"}, {82, "UP ARROW", "\xE2\x86\x91"}, {83, "NUM LOCK", "NUMLK"},
    {84, "NUM /", "/"}, {85, "NUM *", "*"}, {86, "NUM -", "-"}, {87, "NUM +", "+"}, {88, "NUM ENTER", "ENTER"},
    {99, "NUM .", "."}, {101, "MENU KEY", "MENU"},
    {224, "LEFT CTRL", "CTRL"}, {225, "LEFT SHIFT", "SHIFT"}, {226, "LEFT ALT", "ALT"},
    {228, "RIGHT CTRL", "CTRL"}, {229, "RIGHT SHIFT", "SHIFT"}, {230, "RIGHT ALT", "ALT"},
};
const char* const kMouseNames[] = {"", "MOUSE LEFT", "MOUSE MIDDLE", "MOUSE RIGHT", "MOUSE 4", "MOUSE 5"};

std::mutex g_mutex;
Binding g_binds[kActions] = {};
bool g_loaded = false;
std::atomic<u32> g_generation{1};

// capture
std::atomic<int> g_capture{-1};
std::chrono::steady_clock::time_point g_capture_start;
constexpr auto kCaptureTimeout = std::chrono::seconds(5);
bool g_wait_release = false;
bool g_prev[kKeyCount] = {};
u32 g_prev_mouse = 0;
Outcome g_done;
bool g_has_done = false;

void Defaults() {
    for (int i = 0; i < kActions; i++)
        g_binds[i] = kInfo[i].def;
    g_loaded = true;
}
void EnsureLoaded() {
    if (!g_loaded)
        Defaults();
}
std::string Upper(std::string s) {
    for (char& c : s)
        c = char(std::toupper(u8(c)));
    return s;
}
std::string Trim(std::string s) {
    while (!s.empty() && std::isspace(u8(s.back())))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(u8(s[i])))
        i++;
    return s.substr(i);
}
std::u16string U16(const std::string& s) { return std::u16string(s.begin(), s.end()); }

// (g_mutex held)
int AssignLocked(int a, bool mouse, u16 code) {
    EnsureLoaded();
    int swapped = -1;
    for (int i = 0; i < kActions; i++) {
        Binding& o = g_binds[i];
        if (i != a && (mouse ? o.mouse == code : o.key == code)) {
            if (mouse)
                o.mouse = g_binds[a].mouse;
            else
                o.key = g_binds[a].key;
            swapped = i;
        }
    }
    if (mouse)
        g_binds[a].mouse = u8(code);
    else
        g_binds[a].key = code;
    g_generation++;
    return swapped;
}

} // namespace

Binding Get(Action a) {
    std::lock_guard lk(g_mutex);
    EnsureLoaded();
    return g_binds[a];
}
Binding Default(Action a) { return kInfo[a].def; }
void Set(Action a, Binding b) {
    std::lock_guard lk(g_mutex);
    EnsureLoaded();
    g_binds[a] = b;
    g_generation++;
}
void ResetAll() {
    std::lock_guard lk(g_mutex);
    Defaults();
    g_generation++;
}
int Assign(Action a, bool mouse, u16 code) {
    std::lock_guard lk(g_mutex);
    return AssignLocked(a, mouse, code);
}

void Load(const core::Config& c) {
    std::lock_guard lk(g_mutex);
    Defaults();
    for (int i = 0; i < kActions; i++) {
        const auto it = c.keys.find(kInfo[i].id);
        if (it == c.keys.end())
            continue;
        Binding b;
        std::string v = it->second;
        for (size_t p = 0; p <= v.size();) {
            const size_t e = std::min(v.find(',', p), v.size());
            const std::string name = Upper(Trim(v.substr(p, e - p)));
            if (const u8 m = MouseFromName(name))
                b.mouse = m;
            else if (const u16 k = KeyFromName(name))
                b.key = k;
            p = e + 1;
        }
        g_binds[i] = b; // ("NONE": unbound)
    }
    g_generation++;
}
void Store(core::Config& c) {
    std::lock_guard lk(g_mutex);
    EnsureLoaded();
    c.keys.clear();
    for (int i = 0; i < kActions; i++) {
        const Binding& b = g_binds[i];
        if (b == kInfo[i].def)
            continue;
        std::string v = b.key ? KeyName(b.key) : "";
        if (b.mouse)
            v += (v.empty() ? "" : ", ") + MouseName(b.mouse);
        c.keys[kInfo[i].id] = v.empty() ? "NONE" : v;
    }
}

const char* Id(Action a) { return kInfo[a].id; }
std::u16string Label(Action a) { return kInfo[a].label; }

std::string KeyName(u16 key) {
    if (key >= 4 && key <= 29)
        return std::string(1, char('A' + key - 4));
    if (key >= 30 && key <= 39)
        return std::string(1, key == 39 ? '0' : char('1' + key - 30));
    if (key >= 58 && key <= 69)
        return "F" + std::to_string(key - 57);
    if (key >= 89 && key <= 98)
        return "NUM " + std::string(1, key == 98 ? '0' : char('1' + key - 89));
    for (const auto& n : kNames)
        if (n.key == key)
            return n.name;
    return key ? "KEY " + std::to_string(key) : "";
}
std::string KeyIcon(u16 key) {
    if (key >= 89 && key <= 98)
        return std::string(1, key == 98 ? '0' : char('1' + key - 89));
    for (const auto& n : kNames)
        if (n.key == key)
            return n.icon;
    return KeyName(key);
}
std::string MouseName(u8 button) { return button >= 1 && button <= 5 ? kMouseNames[button] : ""; }
u16 KeyFromName(const std::string& name) {
    const std::string n = Upper(Trim(name));
    for (u16 k = 1; k < kKeyCount; k++)
        if (KeyName(k) == n && n.rfind("KEY ", 0) != 0)
            return k;
    if (n.rfind("KEY ", 0) == 0) // an unnamed key by number
        return u16(std::clamp(std::atoi(n.c_str() + 4), 0, kKeyCount - 1));
    return 0;
}
u8 MouseFromName(const std::string& name) {
    const std::string n = Upper(Trim(name));
    for (u8 b = 1; b <= 5; b++)
        if (n == kMouseNames[b])
            return b;
    return 0;
}
std::u16string Text(Binding b) {
    std::string t = b.key ? KeyName(b.key) : "";
    if (b.mouse)
        t += (t.empty() ? "" : "  /  ") + MouseName(b.mouse);
    return t.empty() ? u"---" : U16(t);
}

void Map(const bool* keys, u32 mouse, hid::PadInput& p) {
    Binding binds[kActions];
    {
        std::lock_guard lk(g_mutex);
        EnsureLoaded();
        std::copy(std::begin(g_binds), std::end(g_binds), binds);
    }
    s32* axes[2][2] = {{&p.lx, &p.ly}, {&p.rx, &p.ry}};
    for (int i = 0; i < kActions; i++) {
        const Binding& b = binds[i];
        const bool down = (b.key && b.key < kKeyCount && keys[b.key]) || (b.mouse && (mouse >> (b.mouse - 1)) & 1);
        if (!down)
            continue;
        const Info& in = kInfo[i];
        if (in.button)
            p.buttons |= in.button;
        else
            *axes[in.stick][in.axis] = in.sign * 32767;
    }
}

void StartCapture(Action a) {
    std::lock_guard lk(g_mutex);
    g_has_done = false;
    g_capture = a;
    g_capture_start = std::chrono::steady_clock::now();
}
int Capturing() { return g_capture.load(std::memory_order_relaxed); }

bool Feed(const bool* keys, u32 mouse, u64 pad_buttons) {
    std::lock_guard lk(g_mutex);
    const int a = g_capture.load(std::memory_order_relaxed);
    int key = 0, button = 0;
    for (int k = 1; k < kKeyCount; k++)
        if (keys[k] && !g_prev[k] && !key)
            key = k;
    for (int b = 0; b < 5; b++)
        if ((mouse >> b) & 1 && !((g_prev_mouse >> b) & 1) && !button)
            button = b + 1;
    bool any = mouse != 0 || pad_buttons != 0;
    for (int k = 1; k < kKeyCount && !any; k++)
        any = keys[k];
    std::memcpy(g_prev, keys, sizeof(g_prev));
    g_prev_mouse = mouse;
    if (a < 0) {
        if (g_wait_release && !any)
            g_wait_release = false;
        return g_wait_release;
    }
    const bool timed_out = std::chrono::steady_clock::now() - g_capture_start > kCaptureTimeout;
    if (!key && !button && !(pad_buttons & hid::B) && !timed_out)
        return true;
    Outcome o;
    o.action = a;
    if (key == kEscape || (!key && !button)) // Escape, the pad's B, or 5 s with nothing
        o.cancelled = true;
    else if (key == kBackspace)
        g_binds[a] = {};
    else if (key)
        o.swapped = AssignLocked(a, false, u16(key));
    else
        o.swapped = AssignLocked(a, true, u16(button));
    g_generation++;
    g_done = o;
    g_has_done = true;
    g_capture = -1;
    g_wait_release = true;
    hid::PulseButtons(hid::A, 150);
    return true;
}

bool TakeDone(Outcome& out) {
    std::lock_guard lk(g_mutex);
    if (!g_has_done)
        return false;
    out = g_done;
    g_has_done = false;
    return true;
}

u32 Generation() { return g_generation.load(std::memory_order_relaxed); }

} // namespace hle::keybinds
