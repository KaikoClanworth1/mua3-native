// Button prompt icons per input device (prompts.h). The game's prompts are the 64 x 64 BC3 icons of
// UI/screenlayout/80_button_icon/80_button_icon.ext.kslt (btn_A_icon, btn_ZL_icon, ...; a few layouts carry copies).
// Each icon is known by a hash of its blocks; when the texture cache decodes one (gpu::g_decoded_level_hook) its
// guest storage is remembered and, if another device's set is showing, the blocks are replaced in the decode and in
// guest memory. Switching devices rewrites every remembered icon in guest memory; the texture cache's write tracking
// uploads them again. Our icons are drawn here (shapes plus Windows fonts) in the game's style: a grey button with a
// black outline and a soft drop shadow, the symbol in heavy type with a dark outline.
#include "core/hle/prompts.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include "common/common.h"
#include "core/hle/keybinds.h"
#include "core/hle/menu_label.h"
#include "core/hle/prompt_art.h"
#include "core/hle/mods.h"
#include "gpu/formats.h"

namespace hle::prompts {
namespace {

constexpr int kIcon = 64, kBlocks = kIcon / 4, kBytes = kBlocks * kBlocks * 16;
constexpr int S = 4, W = kIcon * S; // supersampling

using Blocks = std::vector<u8>;

u64 Hash(const u8* p, size_t n) {
    u64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// ---- drawing (hi-res coverage layers, composited and box-filtered to 64 x 64)

struct Layer {
    std::vector<float> a = std::vector<float>(size_t(W) * W, 0.0f);
    float& at(int x, int y) { return a[size_t(y) * W + x]; }
    float get(int x, int y) const { return x < 0 || y < 0 || x >= W || y >= W ? 0.0f : a[size_t(y) * W + x]; }
};

struct Rgb {
    float r, g, b;
};
constexpr Rgb kOutline{0, 0, 0}, kFill{106, 113, 123}, kRim{118, 124, 130}, kInk{246, 234, 0}, kInkEdge{8, 12, 24},
    kHold{255, 92, 16};
// Xbox face buttons
constexpr Rgb kGreen{108, 196, 64}, kRed{236, 64, 52}, kBlue{48, 132, 240}, kYellow{250, 204, 16};

// Shapes in icon pixels (sub-pixel centres)
void Circle(Layer& l, float cx, float cy, float r) {
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++) {
            const float px = (x + 0.5f) / S, py = (y + 0.5f) / S;
            if ((px - cx) * (px - cx) + (py - cy) * (py - cy) <= r * r)
                l.at(x, y) = 1;
        }
}
void RoundRect(Layer& l, float x0, float y0, float x1, float y1, float rad) {
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++) {
            const float px = (x + 0.5f) / S, py = (y + 0.5f) / S;
            const float dx = std::max({x0 + rad - px, 0.0f, px - (x1 - rad)}),
                        dy = std::max({y0 + rad - py, 0.0f, py - (y1 - rad)});
            if (px >= x0 && px <= x1 && py >= y0 && py <= y1 && dx * dx + dy * dy <= rad * rad)
                l.at(x, y) = 1;
        }
}
// Grown by `r` icon pixels (circular)
Layer Dilate(const Layer& in, float r) {
    const int R = int(std::ceil(r * S));
    std::vector<std::pair<int, int>> offs;
    for (int dy = -R; dy <= R; dy++)
        for (int dx = -R; dx <= R; dx++)
            if (dx * dx + dy * dy <= R * R)
                offs.push_back({dx, dy});
    Layer out;
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++) {
            if (in.get(x, y) >= 1) {
                out.at(x, y) = 1;
                continue;
            }
            float m = in.get(x, y);
            for (auto [dx, dy] : offs) {
                m = std::max(m, in.get(x + dx, y + dy));
                if (m >= 1)
                    break;
            }
            out.at(x, y) = m;
        }
    return out;
}

// Text centred on (cx, cy) with capitals `cap` icon pixels high
void Text(Layer& l, const std::wstring& text, float cap, float cx, float cy, const wchar_t* face = L"Segoe UI Black",
          float max_w = 0) {
    const int probe = 200;
    auto probe_cov = menu::TextCoverage(L"H", face, FW_BLACK, false, probe, probe * 2, probe * 2, probe / 2, probe / 2);
    int y0 = probe * 2, y1 = -1;
    for (int y = 0; y < probe * 2; y++)
        for (int x = 0; x < probe * 2; x++)
            if (probe_cov[size_t(y) * probe * 2 + x] > 64)
                y0 = std::min(y0, y), y1 = std::max(y1, y);
    if (y1 < 0)
        return;
    int px = int(std::lround(probe * cap * S / float(y1 - y0 + 1)));
    const int cw = W * 2;
    std::vector<u8> cov;
    int ix0, iy0, ix1, iy1;
    for (int pass = 0; pass < 2; pass++) { // (again smaller if wider than max_w)
        cov = menu::TextCoverage(text, face, FW_BLACK, false, px, cw, cw, cw / 4, cw / 4);
        ix0 = cw, iy0 = cw, ix1 = -1, iy1 = -1;
        for (int y = 0; y < cw; y++)
            for (int x = 0; x < cw; x++)
                if (cov[size_t(y) * cw + x] > 64)
                    ix0 = std::min(ix0, x), ix1 = std::max(ix1, x), iy0 = std::min(iy0, y), iy1 = std::max(iy1, y);
        if (ix1 < 0)
            return;
        if (max_w <= 0 || ix1 - ix0 + 1 <= max_w * S)
            break;
        px = int(px * max_w * S / float(ix1 - ix0 + 1));
    }
    const float mx = (ix0 + ix1 + 1) / 2.0f, my = (iy0 + iy1 + 1) / 2.0f;
    const int dx = int(std::lround(cx * S - mx)), dy = int(std::lround(cy * S - my));
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++) {
            const int sx = x - dx, sy = y - dy;
            if (sx >= 0 && sy >= 0 && sx < cw && sy < cw)
                l.at(x, y) = std::max(l.at(x, y), cov[size_t(sy) * cw + sx] / 255.0f);
        }
}
void Bar(Layer& l, float x0, float y0, float x1, float y1) { RoundRect(l, x0, y0, x1, y1, (y1 - y0) / 2); }

struct Canvas {
    std::vector<float> rgba = std::vector<float>(size_t(W) * W * 4, 0.0f); // straight alpha, 0-255
    void Over(const Layer& l, Rgb c, float alpha = 1, int ox = 0, int oy = 0) {
        for (int y = 0; y < W; y++)
            for (int x = 0; x < W; x++) {
                const float a = l.get(x - ox, y - oy) * alpha;
                if (a <= 0)
                    continue;
                float* p = &rgba[(size_t(y) * W + x) * 4];
                const float da = p[3] / 255.0f, oa = a + da * (1 - a);
                const float src[3] = {c.r, c.g, c.b};
                for (int k = 0; k < 3; k++)
                    p[k] = (src[k] * a + p[k] * da * (1 - a)) / oa;
                p[3] = oa * 255.0f;
            }
    }
    Blocks Encode() const {
        std::vector<float> out(size_t(kIcon) * kIcon * 4, 0.0f);
        for (int y = 0; y < kIcon; y++)
            for (int x = 0; x < kIcon; x++) {
                float acc[4] = {}, wa = 0;
                for (int sy = 0; sy < S; sy++)
                    for (int sx = 0; sx < S; sx++) {
                        const float* p = &rgba[(size_t(y * S + sy) * W + x * S + sx) * 4];
                        for (int k = 0; k < 3; k++)
                            acc[k] += p[k] * p[3];
                        wa += p[3];
                    }
                float* o = &out[(size_t(y) * kIcon + x) * 4];
                for (int k = 0; k < 3; k++)
                    o[k] = wa > 0 ? acc[k] / wa : 0;
                o[3] = wa / (S * S);
            }
        return menu::EncodeBC3Rgba(out, kIcon, kIcon);
    }
};

// A button: its shape (outline grown around it, shadow below right), the symbol in `ink` with a dark edge
Blocks Button(const Layer& shape, const Layer& symbol, Rgb ink, Rgb outline = kOutline) {
    Canvas c;
    const Layer outer = Dilate(shape, 2.0f);
    const Layer shadow = Dilate(outer, 1.0f);
    c.Over(shadow, {0, 0, 0}, 0.30f, 3 * S, 4 * S);
    c.Over(shadow, {0, 0, 0}, 0.25f, 2 * S, 2 * S);
    c.Over(outer, outline);
    c.Over(shape, kRim);
    Layer inner = shape;
    {
        // the rim: the shape less 1.5 px
        Layer inv;
        for (size_t i = 0; i < inv.a.size(); i++)
            inv.a[i] = 1 - shape.a[i];
        const Layer grown = Dilate(inv, 1.5f);
        for (size_t i = 0; i < inner.a.size(); i++)
            inner.a[i] = std::max(0.0f, shape.a[i] - grown.a[i]);
    }
    c.Over(inner, kFill);
    c.Over(Dilate(symbol, 2.0f), kInkEdge);
    c.Over(symbol, ink);
    return c.Encode();
}

constexpr float kCx = 32, kCy = 32, kR = 22; // face buttons (the game's: 48 px across with the outline, centred)

Blocks Face(const std::wstring& letter, Rgb ink) {
    Layer shape, sym;
    Circle(shape, kCx, kCy, kR);
    Text(sym, letter, 22, kCx, kCy);
    return Button(shape, sym, ink);
}
Blocks Shoulder(const std::wstring& text, Rgb ink, bool trigger) {
    Layer shape, sym;
    // where the game's L / ZL sit (outline included: x 6-58, y 13-53 and 12-52); triggers rounder
    if (trigger)
        RoundRect(shape, 8, 14, 56, 50, 9);
    else
        RoundRect(shape, 8, 15, 56, 51, 7);
    Text(sym, text, 19, 32, trigger ? 32 : 33);
    return Button(shape, sym, ink);
}
// Menu (three bars) and View (two overlapping windows), on a button or alone (the _JOY icons)
Layer MenuSymbol(float cx, float cy, float s) {
    Layer l;
    for (int i = -1; i <= 1; i++)
        Bar(l, cx - 9 * s, cy + i * 6.5f * s - 2 * s, cx + 9 * s, cy + i * 6.5f * s + 2 * s);
    return l;
}
Layer ViewSymbol(float cx, float cy, float s) {
    Layer back, front, l;
    RoundRect(back, cx - 10 * s, cy - 9 * s, cx + 4 * s, cy + 3 * s, 2.5f * s);
    RoundRect(front, cx - 4 * s, cy - 3 * s, cx + 10 * s, cy + 9 * s, 2.5f * s);
    Layer hole;
    RoundRect(hole, cx - 7 * s, cy - 6 * s, cx + 1 * s, cy + 0 * s, 1.0f * s);
    for (size_t i = 0; i < l.a.size(); i++)
        l.a[i] = std::max(back.a[i] * (1 - hole.a[i]), front.a[i]);
    Layer front_hole;
    RoundRect(front_hole, cx - 1 * s, cy + 0 * s, cx + 7 * s, cy + 6 * s, 1.0f * s);
    for (size_t i = 0; i < l.a.size(); i++)
        l.a[i] *= 1 - front_hole.a[i];
    return l;
}
Blocks OnButton(const Layer& sym) {
    Layer shape;
    Circle(shape, kCx, kCy, kR);
    return Button(shape, sym, kInk);
}
Blocks Alone(const Layer& sym) { // the _JOY icons: just the symbol, grey with an outline and shadow
    Canvas c;
    const Layer outer = Dilate(sym, 2.0f);
    c.Over(outer, {0, 0, 0}, 0.35f, 2 * S, 3 * S);
    c.Over(outer, kOutline);
    c.Over(sym, kFill);
    return c.Encode();
}
// The four face buttons as a diamond, letters where an Xbox pad has them
Blocks Diamond() {
    Canvas c;
    struct B {
        float x, y;
        const wchar_t* t;
        Rgb ink;
    } bs[] = {{32, 11, L"Y", kYellow}, {11, 32, L"X", kBlue}, {53, 32, L"B", kRed}, {32, 53, L"A", kGreen}};
    Layer shapes, syms;
    for (auto& b : bs)
        Circle(shapes, b.x, b.y, 9.5f);
    const Layer outer = Dilate(shapes, 1.5f);
    c.Over(outer, {0, 0, 0}, 0.35f, 2 * S, 2 * S);
    c.Over(outer, kOutline);
    c.Over(shapes, kFill);
    for (auto& b : bs) {
        Layer s;
        Text(s, b.t, 10, b.x, b.y);
        c.Over(Dilate(s, 1.2f), kInkEdge);
        c.Over(s, b.ink);
    }
    return c.Encode();
}

// A key: a keycap with the key's name (wide for long names)
Blocks KeyCap(const std::wstring& name, Rgb ink) {
    Layer shape, sym;
    const size_t n = name.size();
    if (n >= 4) {
        RoundRect(shape, 5, 14, 59, 50, 7);
        Text(sym, name, 12, 32, 32, L"Segoe UI Black", 46);
    } else {
        RoundRect(shape, 10, 10, 54, 54, 7);
        Text(sym, name, n == 1 ? 22 : 15, 32, 32, L"Segoe UI Black", 36);
    }
    return Button(shape, sym, ink);
}
// A mouse with the button lit (1 left, 2 middle, 3 right); 4 and 5 are keycaps "M4" / "M5"
Blocks MouseIcon(u8 button, Rgb ink) {
    if (button >= 4)
        return KeyCap(button == 4 ? L"M4" : L"M5", ink);
    Layer body;
    RoundRect(body, 18, 7, 46, 57, 14);
    Layer inv;
    for (size_t i = 0; i < inv.a.size(); i++)
        inv.a[i] = 1 - body.a[i];
    const Layer edge = Dilate(inv, 3.0f); // the body less 3 px
    Layer area;
    if (button == 2)
        RoundRect(area, 29.5f, 12, 34.5f, 25, 2.5f);
    else
        RoundRect(area, button == 1 ? 0 : 33.5f, 0, button == 1 ? 30.5f : 64, 27, 0);
    Layer sym;
    for (size_t i = 0; i < sym.a.size(); i++)
        sym.a[i] = std::max(0.0f, std::min(area.a[i], 1 - edge.a[i]));
    return Button(body, sym, ink);
}

std::wstring Wide(const std::string& u8s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8s.data(), int(u8s.size()), nullptr, 0);
    std::wstring w(size_t(std::max(n, 0)), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, u8s.data(), int(u8s.size()), w.data(), n);
    return w;
}

// An icon of prompt_art (64 x 64 RGBA8) as BC3 blocks (empty: none)
Blocks FromArt(const prompt_art::Rgba& art) {
    if (art.size() != size_t(kIcon) * kIcon * 4)
        return {};
    return menu::EncodeBC3Rgba(std::vector<float>(art.begin(), art.end()), kIcon, kIcon);
}

// The icon an action's binding makes (empty: none, the game's own stays): prompt_art's keycap or mouse, else ours
Blocks BindingIcon(keybinds::Action a, bool hold) {
    const keybinds::Binding b = keybinds::Get(a);
    const Rgb ink = hold ? kHold : kInk;
    if (b.key) {
        Blocks art = FromArt(prompt_art::Key(b.key, hold));
        return !art.empty() ? art : KeyCap(Wide(keybinds::KeyIcon(b.key)), ink);
    }
    if (b.mouse) {
        Blocks art = FromArt(prompt_art::Mouse(b.mouse, hold));
        return !art.empty() ? art : MouseIcon(b.mouse, ink);
    }
    return {};
}
// Several directions on one icon ("WASD", the arrows) if each key is one character: four as prompt_art's inverted T
Blocks ClusterIcon(std::initializer_list<keybinds::Action> as) {
    if (as.size() == 4) {
        u16 k[4];
        int i = 0;
        for (keybinds::Action a : as)
            k[i++] = keybinds::Get(a).key;
        if (Blocks art = FromArt(prompt_art::Cluster(k[0], k[1], k[2], k[3])); !art.empty())
            return art;
    }
    std::wstring t;
    for (keybinds::Action a : as) {
        const keybinds::Binding b = keybinds::Get(a);
        const std::wstring k = b.key ? Wide(keybinds::KeyIcon(b.key)) : L"";
        if (k.size() != 1)
            return {};
        t += k;
    }
    return KeyCap(t, kInk);
}

// The keyboard set: the keys bound to each icon's button
std::map<std::string, Blocks> KeyboardIcons() {
    using namespace keybinds;
    std::map<std::string, Blocks> m;
    const struct {
        const char* name;
        Action a;
        bool hold;
    } single[] = {
        {"btn_A_icon", kA, false},      {"btn_A_icon_hold", kA, true}, {"btn_B_icon", kB, false},
        {"btn_X_icon", kX, false},      {"btn_Y_icon", kY, false},     {"btn_Y_icon_hold", kY, true},
        {"btn_L_icon", kL, false},      {"btn_L_icon_hold", kL, true}, {"btn_R_icon", keybinds::kR, false},
        {"btn_ZL_icon", kZL, false},    {"btn_ZR_icon", kZR, false},   {"btn_Plus_icon", kPlus, false},
        {"btn_Plus_JOY_icon", kPlus, false}, {"btn_Minus_icon", kMinus, false}, {"btn_Minus_JOY_icon", kMinus, false},
        {"stick_btn_L_push_icon", kLStick, false}, {"stick_btn_R_push_icon", kRStick, false},
        {"btn_Cross_UP_icon", kDUp, false}, {"btn_Cross_DOWN_icon", kDDown, false},
        {"btn_Cross_L_icon", kDLeft, false}, {"btn_Cross_R_icon", kDRight, false},
        {"btn_dir_UP_icon", kDUp, false}, {"btn_dir_DOWN_icon", kDDown, false},
        {"btn_dir_L_icon", kDLeft, false}, {"btn_dir_L_icon_hold", kDLeft, true},
        {"btn_dir_R_icon", kDRight, false}, {"btn_dir_R_icon_hold", kDRight, true},
    };
    for (const auto& e : single)
        m[e.name] = BindingIcon(e.a, e.hold);
    m["btn_Cross_UPDOWN_icon"] = ClusterIcon({kDUp, kDDown});
    m["btn_Cross_LR_icon"] = ClusterIcon({kDLeft, kDRight});
    m["stick_btn_L_icon"] = ClusterIcon({kMoveUp, kMoveLeft, kMoveDown, kMoveRight});
    m["stick_btn_R_icon"] = ClusterIcon({kCamUp, kCamLeft, kCamDown, kCamRight});
    return m;
}

// ---- the sets

enum Set { kSwitch, kXboxLetters, kXboxPositions, kKeyboard, kPsLetters, kPsPositions, kSets };
const char* const kSetNames[] = {"Switch",   "Xbox (by letter)",        "Xbox (Switch positions)",
                                 "keyboard", "PlayStation (by letter)", "PlayStation (Switch positions)"};

struct Icon {
    std::string name;
    Blocks blocks[kSets]; // empty = the game's own (kSwitch's is always the original)
    u64 hash[kSets] = {};
};

struct Location {
    u8* guest;
    bool block_linear;
    u32 bhl, pitch;
    size_t icon;
    int set; // whose blocks are in guest memory
};

std::mutex g_mutex;
std::vector<Icon> g_icons;
// hash -> (icon, set): one image can be several (Xbox A by letter = Xbox B... no: the game's B by Switch position, the
// same green A; LB in both Xbox sets)
std::unordered_map<u64, std::vector<std::pair<size_t, int>>> g_by_hash;
std::vector<Location> g_locations;
std::map<u32, std::vector<u32>> g_swizzle; // block height log2 -> guest offset of each linear block (64 x 64)
std::atomic<bool> g_ready{false};
Device g_device = Device::Switch;
int g_layout = 0;

int WantedSet() {
    switch (g_device) {
    case Device::Xbox:
        return g_layout == 1 ? kXboxPositions : kXboxLetters;
    case Device::Keyboard:
        return kKeyboard;
    case Device::PlayStation:
        return g_layout == 1 ? kPsPositions : kPsLetters;
    default:
        return kSwitch;
    }
}

const Blocks& BlocksOf(const Icon& ic, int set) { return ic.blocks[set].empty() ? ic.blocks[kSwitch] : ic.blocks[set]; }

const std::vector<u32>& SwizzleMap(u32 bhl) {
    auto it = g_swizzle.find(bhl);
    if (it != g_swizzle.end())
        return it->second;
    const u64 size = gpu::BlockLinearSize(kBlocks, kBlocks, 1, 16, bhl, 0);
    std::vector<u8> idx(size_t(size), 0), lin(kBytes);
    for (u32 i = 0; i < size / 16; i++)
        std::memcpy(&idx[size_t(i) * 16], &i, 4);
    gpu::Deswizzle(idx.data(), lin.data(), kBlocks, kBlocks, 1, 16, bhl, 0, kBlocks * 16);
    std::vector<u32> map(kBlocks * kBlocks);
    for (size_t k = 0; k < map.size(); k++) {
        u32 i;
        std::memcpy(&i, &lin[k * 16], 4);
        map[k] = i * 16;
    }
    return g_swizzle[bhl] = std::move(map);
}

// Which icon and set an image is: the one of `icon` if it can be (SIZE_MAX: any), else the wanted set's, else the first
bool Match(u64 h, size_t icon, std::pair<size_t, int>& out) {
    const auto it = g_by_hash.find(h);
    if (it == g_by_hash.end())
        return false;
    const int want = WantedSet();
    const std::pair<size_t, int>* best = nullptr;
    for (const auto& c : it->second) {
        if (icon != SIZE_MAX && c.first != icon)
            continue;
        if (!best || c.second == want)
            best = &c;
    }
    if (!best)
        return false;
    out = *best;
    return true;
}

u8* BlockAt(const Location& l, size_t k) {
    if (l.block_linear)
        return l.guest + SwizzleMap(l.bhl)[k];
    return l.guest + (k / kBlocks) * l.pitch + (k % kBlocks) * 16;
}

u64 GuestHash(const Location& l) {
    Blocks b(kBytes);
    for (size_t k = 0; k < size_t(kBlocks) * kBlocks; k++)
        std::memcpy(&b[k * 16], BlockAt(l, k), 16);
    return Hash(b.data(), b.size());
}

bool ShowsDifferent(const Icon& ic, int a, int b) { return BlocksOf(ic, a) != BlocksOf(ic, b); }

// Writes a set's icon into guest memory (the tracked pages fault and are marked dirty: the renderer uploads again)
void Write(Location& l, int set) {
    const Blocks& b = BlocksOf(g_icons[l.icon], set);
    for (size_t k = 0; k < size_t(kBlocks) * kBlocks; k++)
        std::memcpy(BlockAt(l, k), &b[k * 16], 16);
    l.set = set;
}

void DecodedLevel(u8* guest, u8* lin, u32 bw, u32 bh, bool block_linear, u32 bhl, u32 pitch) {
    if (bw != kBlocks || bh != kBlocks)
        return;
    const u64 h = Hash(lin, kBytes);
    std::lock_guard lk(g_mutex);
    auto loc = std::find_if(g_locations.begin(), g_locations.end(), [&](const Location& l) { return l.guest == guest; });
    std::pair<size_t, int> m;
    if (!(loc != g_locations.end() && Match(h, loc->icon, m)) && !Match(h, SIZE_MAX, m))
        return;
    const auto [icon, set] = m;
    if (loc == g_locations.end()) {
        g_locations.push_back({guest, block_linear, bhl, pitch, icon, set});
        loc = g_locations.end() - 1;
    } else {
        loc->icon = icon, loc->set = set, loc->block_linear = block_linear, loc->bhl = bhl, loc->pitch = pitch;
    }
    const int want = g_ready ? WantedSet() : kSwitch;
    if (want != set && g_ready && ShowsDifferent(g_icons[icon], set, want)) {
        std::memcpy(lin, BlocksOf(g_icons[icon], want).data(), kBytes);
        Write(*loc, want);
    }
}

// Rewrites every icon seen so far with the wanted set (only where guest memory still holds one of our sets' icon)
void Apply() {
    std::lock_guard lk(g_mutex);
    if (!g_ready)
        return;
    const int want = WantedSet();
    size_t n = 0, gone = 0;
    for (auto it = g_locations.begin(); it != g_locations.end();) {
        Location& l = *it;
        const u64 h = GuestHash(l);
        std::pair<size_t, int> m;
        if (!Match(h, l.icon, m)) { // the memory holds something else now
            it = g_locations.erase(it), gone++;
            continue;
        }
        l.set = m.second;
        if (l.set != want && ShowsDifferent(g_icons[l.icon], l.set, want)) {
            Write(l, want), n++;
            if (GuestHash(l) != Hash(BlocksOf(g_icons[l.icon], want).data(), kBytes))
                LOG_WARN("Prompts", "{}: guest memory differs after the write", g_icons[l.icon].name);
        }
        ++it;
    }
    LOG_INFO("Prompts", "button icons: {} ({} rewritten, {} no longer loaded)", kSetNames[want], n, gone);
}

bool ReadPack(std::vector<std::pair<std::string, Blocks>>& out) {
    const auto d = common::ReadFile(hle::ResolveRomfs("UI/screenlayout/80_button_icon/80_button_icon.ext.kslt"));
    auto u32at = [&](size_t o) { u32 v = 0; if (o + 4 <= d.size()) std::memcpy(&v, &d[o], 4); return v; };
    if (d.size() < 0x40 || std::memcmp(d.data(), "TLSK3100", 8) != 0)
        return false;
    const u32 n = u32at(8), params = u32at(0x10);
    const size_t offs = 0x40 + size_t(params);
    size_t p = offs + 0x14 * size_t(n);
    for (u32 i = 0; i < n && p < d.size(); i++) {
        const size_t e = std::find(d.begin() + p, d.end(), u8(0)) - d.begin();
        std::string name(d.begin() + p, d.begin() + e);
        p = e + 1;
        const size_t at = u32at(offs + 0x14 * i);
        if (at + 0x48 > d.size())
            continue;
        u16 w, h;
        std::memcpy(&w, &d[at + 4], 2);
        std::memcpy(&h, &d[at + 6], 2);
        const u32 size = u32at(at + 0x1c);
        if (w != kIcon || h != kIcon || size != kBytes || at + 0x48 + size > d.size())
            continue;
        out.push_back({std::move(name), Blocks(d.begin() + at + 0x48, d.begin() + at + 0x48 + size)});
    }
    return !out.empty();
}

void Build() {
    // Xbox: by letter the game's A is Xbox A; by Switch position it is the right button, Xbox B (and so on)
    const struct {
        const char* game;
        const wchar_t* letter[2]; // by letter, by position
        Rgb ink[2];
    } faces[] = {
        {"btn_A_icon", {L"A", L"B"}, {kGreen, kRed}},
        {"btn_B_icon", {L"B", L"A"}, {kRed, kGreen}},
        {"btn_X_icon", {L"X", L"Y"}, {kBlue, kYellow}},
        {"btn_Y_icon", {L"Y", L"X"}, {kYellow, kBlue}},
    };
    std::map<std::string, Blocks> xbox[2];
    for (int lay = 0; lay < 2; lay++) {
        for (auto& f : faces) {
            xbox[lay][f.game] = Face(f.letter[lay], f.ink[lay]);
            xbox[lay][std::string(f.game) + "_hold"] = Face(f.letter[lay], kHold);
        }
    }
    std::map<std::string, Blocks> common = {
        {"btn_L_icon", Shoulder(L"LB", kInk, false)},     {"btn_L_icon_hold", Shoulder(L"LB", kHold, false)},
        {"btn_R_icon", Shoulder(L"RB", kInk, false)},     {"btn_ZL_icon", Shoulder(L"LT", kInk, true)},
        {"btn_ZR_icon", Shoulder(L"RT", kInk, true)},     {"btn_Plus_icon", OnButton(MenuSymbol(kCx, kCy, 1))},
        {"btn_Minus_icon", OnButton(ViewSymbol(kCx, kCy, 1))}, {"btn_Plus_JOY_icon", Alone(MenuSymbol(32, 32, 1.1f))},
        {"btn_Minus_JOY_icon", Alone(ViewSymbol(32, 32, 1.1f))}, {"btn_dir_all_ABXY_icon", Diamond()},
    };
    // prompt_art's Xbox and PlayStation icons first (ours above stay the Xbox fallback)
    std::vector<std::string> names;
    {
        std::lock_guard lk(g_mutex);
        for (const Icon& ic : g_icons)
            names.push_back(ic.name);
    }
    std::map<std::string, Blocks> art[kSets];
    for (const std::string& n : names)
        for (int lay = 0; lay < 2; lay++) {
            art[lay ? kXboxPositions : kXboxLetters][n] = FromArt(prompt_art::PadIcon(prompt_art::Pad::Xbox, n, lay));
            art[lay ? kPsPositions : kPsLetters][n] = FromArt(prompt_art::PadIcon(prompt_art::Pad::PlayStation, n, lay));
        }
    std::lock_guard lk(g_mutex);
    for (Icon& ic : g_icons) {
        for (int lay = 0; lay < 2; lay++) {
            const int set = lay ? kXboxPositions : kXboxLetters;
            if (Blocks& a = art[set][ic.name]; !a.empty())
                ic.blocks[set] = std::move(a);
            else if (auto it = xbox[lay].find(ic.name); it != xbox[lay].end())
                ic.blocks[set] = it->second;
            else if (auto c = common.find(ic.name); c != common.end())
                ic.blocks[set] = c->second;
            const int ps = lay ? kPsPositions : kPsLetters;
            ic.blocks[ps] = std::move(art[ps][ic.name]);
        }
        for (int set = 1; set < kSets; set++)
            if (set != kKeyboard && !ic.blocks[set].empty()) {
                ic.hash[set] = Hash(ic.blocks[set].data(), kBytes);
                g_by_hash[ic.hash[set]].push_back({size_t(&ic - g_icons.data()), set});
            }
    }
    g_ready = true;
}

// The keyboard set drawn again from the bindings (off the game's threads), then put in: the icons showing the old
// keyboard set are rewritten.
std::atomic<u32> g_keys_wanted{0};
void BuildKeyboard(u32 gen) {
    std::map<std::string, Blocks> keys = KeyboardIcons();
    std::lock_guard lk(g_mutex);
    if (gen != g_keys_wanted.load())
        return; // a newer one is on its way
    for (auto& [h, cands] : g_by_hash)
        std::erase_if(cands, [](const std::pair<size_t, int>& c) { return c.second == kKeyboard; });
    for (Icon& ic : g_icons) {
        auto it = keys.find(ic.name);
        ic.blocks[kKeyboard] = it != keys.end() ? std::move(it->second) : Blocks{};
        ic.hash[kKeyboard] = 0;
        if (!ic.blocks[kKeyboard].empty()) {
            ic.hash[kKeyboard] = Hash(ic.blocks[kKeyboard].data(), kBytes);
            g_by_hash[ic.hash[kKeyboard]].push_back({size_t(&ic - g_icons.data()), int(kKeyboard)});
        }
    }
    const int want = WantedSet();
    size_t n = 0;
    for (Location& l : g_locations)
        if (l.set == kKeyboard) // (its old image is no longer known: written by what we know of the location)
            Write(l, want), n++;
    LOG_INFO("Prompts", "keyboard icons drawn ({} rewritten)", n);
}

} // namespace

void KeysChanged() {
    const u32 gen = ++g_keys_wanted;
    if (!g_ready)
        return; // (Init's thread draws them)
    std::thread([gen] {
        BuildKeyboard(gen);
        Apply();
    }).detach();
}

void Init() {
    std::vector<std::pair<std::string, Blocks>> pack;
    if (!ReadPack(pack)) {
        LOG_WARN("Prompts", "button icon pack not found: prompts stay Switch buttons");
        return;
    }
    {
        std::lock_guard lk(g_mutex);
        for (auto& [name, b] : pack) {
            Icon ic;
            ic.name = name;
            ic.hash[kSwitch] = Hash(b.data(), b.size());
            ic.blocks[kSwitch] = std::move(b);
            g_icons.push_back(std::move(ic));
        }
        for (size_t i = 0; i < g_icons.size(); i++)
            g_by_hash[g_icons[i].hash[kSwitch]].push_back({i, int(kSwitch)});
    }
    gpu::g_decoded_level_hook = &DecodedLevel;
    std::thread([n = g_icons.size()] {
        const auto t0 = std::chrono::steady_clock::now();
        Build();
        BuildKeyboard(g_keys_wanted.load());
        LOG_INFO("Prompts", "{} button icons, pad and keyboard sets drawn in {} ms", n,
                 std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        // test aid: MUA3_PROMPTS_DEVICE=ps / xbox / keyboard shows that device's icons from the start
        if (const char* d = std::getenv("MUA3_PROMPTS_DEVICE")) {
            const std::string v = d;
            std::lock_guard lk(g_mutex);
            g_device = v == "ps" ? Device::PlayStation : v == "xbox" ? Device::Xbox : v == "keyboard" ? Device::Keyboard
                                                                                                      : g_device;
        }
        Apply();
    }).detach();
}

void SetDevice(Device d) {
    {
        std::lock_guard lk(g_mutex);
        if (d == g_device)
            return;
        g_device = d;
    }
    LOG_INFO("Prompts", "input device: {}", DeviceName(d));
    Apply();
}

void SetLayout(int layout) {
    {
        std::lock_guard lk(g_mutex);
        if (layout == g_layout)
            return;
        g_layout = layout;
    }
    Apply();
}

Device CurrentDevice() {
    std::lock_guard lk(g_mutex);
    return g_device;
}

const char* DeviceName(Device d) {
    switch (d) {
    case Device::Xbox: return "Xbox";
    case Device::PlayStation: return "PlayStation";
    case Device::Keyboard: return "keyboard";
    default: return "Switch";
    }
}

} // namespace hle::prompts
