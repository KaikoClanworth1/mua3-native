// Button prompt art (prompt_art.h): unpacks the icons of prompt_art_data.cpp on first use and hands them out.
#include "core/hle/prompt_art.h"

#include <cstring>
#include <mutex>
#include <string>

#include "core/hle/prompt_art_data.h"

namespace hle::prompt_art {
namespace {

constexpr size_t kIconBytes = size_t(kIconSize) * kIconSize * 4;

// LZ4 block format (a token, literals, a 16-bit distance, a match); false if the data is not what was packed
bool Unpack(const u8* src, size_t n, std::vector<u8>& out, size_t size) {
    out.clear();
    out.reserve(size);
    size_t i = 0;
    auto more = [&](size_t& len) {
        for (u8 b = 255; b == 255;) {
            if (i >= n)
                return false;
            b = src[i++];
            len += b;
        }
        return true;
    };
    while (i < n) {
        const u8 tok = src[i++];
        size_t lit = tok >> 4;
        if (lit == 15 && !more(lit))
            return false;
        if (i + lit > n || out.size() + lit > size)
            return false;
        out.insert(out.end(), src + i, src + i + lit);
        i += lit;
        if (i >= n)
            break;
        if (i + 2 > n)
            return false;
        const size_t off = size_t(src[i]) | size_t(src[i + 1]) << 8;
        i += 2;
        size_t len = tok & 15;
        if (len == 15 && !more(len))
            return false;
        len += 4;
        if (off == 0 || off > out.size() || out.size() + len > size)
            return false;
        const size_t at = out.size();
        out.resize(at + len);
        for (size_t k = 0; k < len; k++) // (may overlap itself: byte by byte)
            out[at + k] = out[at + k - off];
    }
    return out.size() == size;
}

const std::vector<u8>& Raw() {
    static std::vector<u8> raw;
    static std::once_flag once;
    std::call_once(once, [] {
        if (!Unpack(data::kPacked, data::kPackedSize, raw, data::kRawSize)) {
            LOG_WARN("Prompts", "prompt art did not unpack: the drawn fallback icons are used");
            raw.clear();
        }
    });
    return raw;
}

Rgba Icon(u32 index) {
    const std::vector<u8>& raw = Raw();
    if (index >= data::kIconCount || raw.empty())
        return {};
    const u8* p = raw.data() + size_t(index) * kIconBytes;
    return Rgba(p, p + kIconBytes);
}

// The small key's tile (nullptr: none)
const u8* Mini(u16 usage, bool hold) {
    const std::vector<u8>& raw = Raw();
    if (raw.empty())
        return nullptr;
    for (u32 i = 0; i < data::kMiniCount; i++)
        if (data::kMiniUsages[i] == usage)
            return raw.data() + data::kMiniOffset + (size_t(i) * 2 + (hold ? 1 : 0)) * data::kMiniW * data::kMiniH * 4;
    return nullptr;
}

struct Map {
    const char* game;
    const char* ours[2]; // by layout (nullptr: the same as layout 0)
};

// The game's icon -> ours, for each pad ("%" is the pad's folder; "_hold" is carried over)
constexpr Map kXbox[] = {
    {"btn_A_icon", {"xbox/a", "xbox/b"}},        {"btn_B_icon", {"xbox/b", "xbox/a"}},
    {"btn_X_icon", {"xbox/x", "xbox/y"}},        {"btn_Y_icon", {"xbox/y", "xbox/x"}},
    {"btn_L_icon", {"xbox/lb"}},                 {"btn_R_icon", {"xbox/rb"}},
    {"btn_ZL_icon", {"xbox/lt"}},                {"btn_ZR_icon", {"xbox/rt"}},
    {"btn_Plus_icon", {"xbox/menu"}},            {"btn_Minus_icon", {"xbox/view"}},
    {"btn_Plus_JOY_icon", {"xbox/menu_alone"}},  {"btn_Minus_JOY_icon", {"xbox/view_alone"}},
    {"btn_Cross_empty_icon", {"xbox/dpad"}},     {"btn_Cross_UP_icon", {"xbox/dpad_up"}},
    {"btn_Cross_DOWN_icon", {"xbox/dpad_down"}}, {"btn_Cross_L_icon", {"xbox/dpad_left"}},
    {"btn_Cross_R_icon", {"xbox/dpad_right"}},   {"btn_Cross_LR_icon", {"xbox/dpad_lr"}},
    {"btn_Cross_UPDOWN_icon", {"xbox/dpad_ud"}}, {"btn_dir_all_ABXY_icon", {"xbox/diamond"}},
};
constexpr Map kPlayStation[] = {
    {"btn_A_icon", {"ps/cross", "ps/circle"}},   {"btn_B_icon", {"ps/circle", "ps/cross"}},
    {"btn_X_icon", {"ps/square", "ps/triangle"}}, {"btn_Y_icon", {"ps/triangle", "ps/square"}},
    {"btn_L_icon", {"ps/l1"}},                   {"btn_R_icon", {"ps/r1"}},
    {"btn_ZL_icon", {"ps/l2"}},                  {"btn_ZR_icon", {"ps/r2"}},
    {"btn_Plus_icon", {"ps/options"}},           {"btn_Minus_icon", {"ps/create"}},
    {"btn_Plus_JOY_icon", {"ps/options_alone"}}, {"btn_Minus_JOY_icon", {"ps/create_alone"}},
    {"btn_Cross_empty_icon", {"ps/dpad"}},       {"btn_Cross_UP_icon", {"ps/dpad_up"}},
    {"btn_Cross_DOWN_icon", {"ps/dpad_down"}},   {"btn_Cross_L_icon", {"ps/dpad_left"}},
    {"btn_Cross_R_icon", {"ps/dpad_right"}},     {"btn_Cross_LR_icon", {"ps/dpad_lr"}},
    {"btn_Cross_UPDOWN_icon", {"ps/dpad_ud"}},   {"btn_dir_all_ABXY_icon", {"ps/diamond"}},
};
// The sticks are the same on both pads
constexpr Map kSticks[] = {
    {"stick_btn_L_icon", {"pad/ls"}},            {"stick_btn_R_icon", {"pad/rs"}},
    {"stick_btn_icon", {"pad/stick"}},           {"stick_btn_L_push_icon", {"pad/ls_push"}},
    {"stick_btn_R_push_icon", {"pad/rs_push"}},  {"stick_btn_push_icon", {"pad/stick_push"}},
    {"stick_L_roll_L_icon", {"pad/ls_roll_l"}},  {"stick_L_roll_R_icon", {"pad/ls_roll_r"}},
    {"stick_R_roll_L_icon", {"pad/rs_roll_l"}},  {"stick_R_roll_R_icon", {"pad/rs_roll_r"}},
    {"stick_roll_L_icon", {"pad/stick_roll_l"}}, {"stick_roll_R_icon", {"pad/stick_roll_r"}},
};

template <size_t N>
const char* Find(const Map (&maps)[N], std::string_view game, int layout) {
    for (const Map& m : maps)
        if (game == m.game)
            return layout == 1 && m.ours[1] ? m.ours[1] : m.ours[0];
    return nullptr;
}

} // namespace

Rgba Key(u16 hid_usage, bool hold) {
    for (u32 i = 0; i < data::kKeyCount; i++)
        if (data::kKeys[i].usage == hid_usage)
            return Icon(u32(data::kKeys[i].icon) + (hold ? 1 : 0));
    return {};
}

Rgba Mouse(u8 button, bool hold) {
    if (button >= data::kMouseCount)
        return {};
    return Icon(u32(data::kMouse[button]) + (hold ? 1 : 0));
}

Rgba Cluster(u16 up, u16 left, u16 down, u16 right, bool hold) {
    const u16 usages[4] = {up, left, down, right};
    const u8* tiles[4];
    for (int k = 0; k < 4; k++) {
        tiles[k] = Mini(usages[k], hold);
        if (!tiles[k])
            return {};
    }
    Rgba out(kIconBytes, 0);
    const int w = int(data::kMiniW), h = int(data::kMiniH);
    for (int k = 0; k < 4; k++) {
        for (int ty = 0; ty < h; ty++) {
            for (int tx = 0; tx < w; tx++) {
                const int x = data::kMiniSpots[k].x + tx, y = data::kMiniSpots[k].y + ty;
                if (x < 0 || y < 0 || x >= kIconSize || y >= kIconSize)
                    continue;
                const u8* s = tiles[k] + (size_t(ty) * w + tx) * 4;
                if (!s[3])
                    continue;
                u8* d = &out[(size_t(y) * kIconSize + x) * 4];
                // straight-alpha "over"
                const float sa = s[3] / 255.0f, da = d[3] / 255.0f, oa = sa + da * (1 - sa);
                for (int c = 0; c < 3; c++)
                    d[c] = u8((s[c] * sa + d[c] * da * (1 - sa)) / oa + 0.5f);
                d[3] = u8(oa * 255.0f + 0.5f);
            }
        }
    }
    return out;
}

Rgba Named(std::string_view name) {
    for (u32 i = 0; i < data::kNamedCount; i++)
        if (name == data::kNamed[i].name)
            return Icon(data::kNamed[i].icon);
    return {};
}

Rgba PadIcon(Pad pad, std::string_view game_icon, int layout) {
    constexpr std::string_view kHold = "_hold";
    const bool hold = game_icon.size() > kHold.size() && game_icon.substr(game_icon.size() - kHold.size()) == kHold;
    const std::string_view base = hold ? game_icon.substr(0, game_icon.size() - kHold.size()) : game_icon;
    const char* ours = pad == Pad::Xbox ? Find(kXbox, base, layout) : Find(kPlayStation, base, layout);
    if (!ours)
        ours = Find(kSticks, base, layout);
    if (!ours)
        return {};
    if (!hold)
        return Named(ours);
    Rgba held = Named(std::string(ours) + "_hold");
    return held.empty() ? Named(ours) : held; // (no orange version: the plain one)
}

} // namespace hle::prompt_art
