// Menu labels drawn on the player's PC in the style of the game's pre-rendered GText_* textures (heavy italic
// capitals with a soft drop shadow; white, or a yellow gradient when selected), for menu options the game has no
// label for. Rendered with Windows fonts (GDI) and BC3-encoded like the game's own label textures.
#include "core/hle/menu_label.h"

#include <algorithm>
#include <cmath>

#include <windows.h>

namespace hle::menu {
namespace {

constexpr int kSuper = 4; // supersampling

struct Mask {
    int w = 0, h = 0;
    std::vector<u8> a;
    u8 at(int x, int y) const { return x < 0 || y < 0 || x >= w || y >= h ? 0 : a[size_t(y) * w + x]; }
};

// White-on-black text coverage (GDI antialiasing) on a w x h canvas with the text origin (top-left of the cell) at
// (x, y).
Mask Render(const std::wstring& text, const wchar_t* face, int weight, bool italic, int px, int w, int h, int x, int y) {
    Mask m;
    m.w = w, m.h = h;
    m.a.assign(size_t(w) * h, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        DeleteDC(dc);
        return m;
    }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    std::memset(bits, 0, size_t(w) * h * 4);
    HFONT font = CreateFontW(-px, 0, 0, 0, weight, italic, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
    HGDIOBJ old_font = SelectObject(dc, font);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    TextOutW(dc, x, y, text.c_str(), int(text.size()));
    GdiFlush();
    const u8* p = static_cast<const u8*>(bits);
    for (size_t i = 0; i < m.a.size(); i++)
        m.a[i] = std::max({p[i * 4], p[i * 4 + 1], p[i * 4 + 2]});
    SelectObject(dc, old_font);
    DeleteObject(font);
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return m;
}

// Ink rows/columns (coverage > 1/4).
bool Ink(const Mask& m, int& x0, int& y0, int& x1, int& y1) {
    x0 = m.w, y0 = m.h, x1 = -1, y1 = -1;
    for (int y = 0; y < m.h; y++)
        for (int x = 0; x < m.w; x++)
            if (m.a[size_t(y) * m.w + x] > 64)
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    return x1 >= 0;
}

// ---- BC3 (DXT5) encoding: alpha endpoints = block min/max, colour endpoints = per-channel min/max
u16 To565(const float c[3]) {
    auto q = [](float v, int bits) { return int(std::lround(std::clamp(v, 0.0f, 255.0f) * ((1 << bits) - 1) / 255.0f)); };
    return u16(q(c[0], 5) << 11 | q(c[1], 6) << 5 | q(c[2], 5));
}
void From565(u16 v, float c[3]) {
    c[0] = float((v >> 11) & 31) * 255 / 31;
    c[1] = float((v >> 5) & 63) * 255 / 63;
    c[2] = float(v & 31) * 255 / 31;
}
void EncodeBlock(const float px[16][4], u8 out[16]) {
    // alpha
    float amin = 255, amax = 0;
    for (int i = 0; i < 16; i++)
        amin = std::min(amin, px[i][3]), amax = std::max(amax, px[i][3]);
    u8 a0 = u8(std::lround(amax)), a1 = u8(std::lround(amin));
    float al[8];
    u64 abits = 0;
    if (a0 == a1) {
        out[0] = a0, out[1] = a1; // 6-value mode with equal endpoints: every index decodes to a0
    } else {
        out[0] = a0, out[1] = a1;
        al[0] = a0, al[1] = a1;
        for (int i = 2; i < 8; i++)
            al[i] = ((8 - i) * a0 + (i - 1) * a1) / 7.0f;
        for (int i = 0; i < 16; i++) {
            int best = 0;
            for (int k = 1; k < 8; k++)
                if (std::fabs(al[k] - px[i][3]) < std::fabs(al[best] - px[i][3]))
                    best = k;
            abits |= u64(best) << (3 * i);
        }
    }
    for (int k = 0; k < 6; k++)
        out[2 + k] = u8(abits >> (8 * k));
    // colour (weighted by alpha so invisible pixels do not stretch the endpoints)
    float lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    bool any = false;
    for (int i = 0; i < 16; i++)
        if (px[i][3] > 8) {
            any = true;
            for (int c = 0; c < 3; c++)
                lo[c] = std::min(lo[c], px[i][c]), hi[c] = std::max(hi[c], px[i][c]);
        }
    if (!any)
        lo[0] = lo[1] = lo[2] = hi[0] = hi[1] = hi[2] = 0;
    u16 c0 = To565(hi), c1 = To565(lo);
    if (c0 < c1)
        std::swap(c0, c1);
    u32 cbits = 0;
    if (c0 != c1) {
        float e[4][3];
        From565(c0, e[0]);
        From565(c1, e[1]);
        for (int c = 0; c < 3; c++)
            e[2][c] = (2 * e[0][c] + e[1][c]) / 3, e[3][c] = (e[0][c] + 2 * e[1][c]) / 3;
        for (int i = 0; i < 16; i++) {
            int best = 0;
            float bd = 1e30f;
            for (int k = 0; k < 4; k++) {
                float dd = 0;
                for (int c = 0; c < 3; c++)
                    dd += (e[k][c] - px[i][c]) * (e[k][c] - px[i][c]);
                if (dd < bd)
                    bd = dd, best = k;
            }
            cbits |= u32(best) << (2 * i);
        }
    }
    out[8] = u8(c0), out[9] = u8(c0 >> 8), out[10] = u8(c1), out[11] = u8(c1 >> 8);
    for (int k = 0; k < 4; k++)
        out[12 + k] = u8(cbits >> (8 * k));
}

} // namespace

std::vector<u8> RenderLabel(const LabelStyle& st, const std::wstring& text, const wchar_t* face, bool cjk) {
    const int S = kSuper, W = st.w * S, H = st.h * S;
    const int weight = cjk ? FW_BOLD : FW_BLACK;
    // font size: the capital height (CJK: the text's own ink height) matches the game's labels
    const std::wstring probe = cjk ? text : L"H";
    const int test_px = 100 * S;
    Mask t = Render(probe, face, weight, true, test_px, test_px * 4, test_px * 2, test_px / 2, test_px / 4);
    int ix0, iy0, ix1, iy1;
    if (!Ink(t, ix0, iy0, ix1, iy1))
        return {};
    const double target = st.cap * S * (cjk ? 1.1 : 1.0);
    const int px = int(std::lround(test_px * target / double(iy1 - iy0 + 1)));
    // where the probe's ink top lands for that size, relative to the text origin
    const int pad = 2 * px;
    Mask p = Render(probe, face, weight, true, px, px * 4 + pad, px * 2 + pad, pad / 2, pad / 2);
    if (!Ink(p, ix0, iy0, ix1, iy1))
        return {};
    const int top_off = iy0 - pad / 2;
    // the text, then its ink moved to the label's left margin and capital line
    Mask m = Render(text, face, weight, true, px, W + pad, H + pad, pad / 2, pad / 2);
    int mx0, my0, mx1, my1;
    if (!Ink(m, mx0, my0, mx1, my1))
        return {};
    const int dx = st.left * S - mx0, dy = st.top * S - (pad / 2 + top_off);
    // a little more slant than the font's own italic (the game's lettering leans further)
    const double shear = 0.10;
    const int base = st.top * S + int(target);
    std::vector<float> cov(size_t(st.w) * st.h, 0.0f);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const int sx = x - dx - int(std::lround(shear * (base - y))), sy = y - dy;
            cov[size_t(y / S) * st.w + x / S] += m.at(sx, sy);
        }
    for (float& c : cov)
        c /= 255.0f * S * S;
    // drop shadow: the coverage blurred and offset down-right
    std::vector<float> sh(cov.size(), 0.0f);
    const int off = std::max(1, st.cap / 12);
    for (int y = 0; y < st.h; y++)
        for (int x = 0; x < st.w; x++) {
            float s = 0, n = 0;
            for (int ky = -2; ky <= 2; ky++)
                for (int kx = -2; kx <= 2; kx++) {
                    const int sx = x - off + kx, sy = y - off + ky;
                    const float wgt = 1.0f / (1 + kx * kx + ky * ky);
                    if (sx >= 0 && sy >= 0 && sx < st.w && sy < st.h)
                        s += cov[size_t(sy) * st.w + sx] * wgt;
                    n += wgt;
                }
            sh[size_t(y) * st.w + x] = 0.75f * s / n;
        }
    // compose (straight alpha): text over shadow
    std::vector<float> rgba(cov.size() * 4);
    for (int y = 0; y < st.h; y++) {
        const float g = std::clamp(float(y - st.top) / float(st.cap), 0.0f, 1.0f);
        const float fill[3] = {st.selected ? 255 - 9 * g : 249.0f, st.selected ? 254 - 44 * g : 248.0f,
                               st.selected ? 85 - 77 * g : 249.0f};
        for (int x = 0; x < st.w; x++) {
            const size_t i = size_t(y) * st.w + x;
            const float a = cov[i], s = sh[i] * (1 - a), A = a + s;
            for (int c = 0; c < 3; c++)
                rgba[i * 4 + c] = A > 0 ? fill[c] * a / A : 0;
            rgba[i * 4 + 3] = std::clamp(A, 0.0f, 1.0f) * 255;
        }
    }
    // BC3, linear block order (as the game's packs store their label textures)
    const int bw = (st.w + 3) / 4, bh = (st.h + 3) / 4;
    std::vector<u8> out(size_t(bw) * bh * 16);
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            float px16[16][4];
            for (int k = 0; k < 16; k++) {
                const int x = std::min(bx * 4 + k % 4, st.w - 1), y = std::min(by * 4 + k / 4, st.h - 1);
                for (int c = 0; c < 4; c++)
                    px16[k][c] = rgba[(size_t(y) * st.w + x) * 4 + c];
            }
            EncodeBlock(px16, &out[(size_t(by) * bw + bx) * 16]);
        }
    return out;
}

} // namespace hle::menu
