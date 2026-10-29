#include "shared/ui/renderer/Renderer.h"

#include <d3d9.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/game/Game.h"

namespace coop::ui {

Renderer& Gfx() {
    static Renderer r;
    return r;
}

std::wstring Widen(std::string_view s) { return Utf8ToWide(s); }
std::string Narrow(std::wstring_view s) { return WideToUtf8(s); }

static constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// Characters baked into the atlases.
static void CollectCharset(std::vector<wchar_t>& out) {
    for (wchar_t c = 0x20; c <= 0x7E; ++c) out.push_back(c);
    for (wchar_t c = 0xA0; c <= 0xFF; ++c) out.push_back(c);
    for (wchar_t c = 0x400; c <= 0x45F; ++c) out.push_back(c);  // Cyrillic
    for (wchar_t c = 0x490; c <= 0x491; ++c) out.push_back(c);  // Ґ ґ
    const wchar_t extra[] = {0x2010, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x201E, 0x2022, 0x2026,
                             0x2116, 0x20AC, 0x2190, 0x2191, 0x2192, 0x2193, 0x25B2, 0x25BC, 0x25CF, 0x2713};
    for (wchar_t c : extra) out.push_back(c);
}

bool Renderer::BuildFont(Font& f, const wchar_t* face, int pixelHeight, int weight, float stretch, float tracking,
                         bool upper) {
    std::vector<wchar_t> chars;
    CollectCharset(chars);
    // an upper-case font draws every letter as a capital (Shape): its small letters are never looked up
    if (upper)
        chars.erase(std::remove_if(chars.begin(), chars.end(),
                                   [](wchar_t c) { return (wchar_t)(uintptr_t)CharUpperW((LPWSTR)(uintptr_t)c) != c; }),
                    chars.end());

    // The atlas grows as wide as the card allows, up to 4096: one 1024 wide could not hold the big font on a 4K screen,
    // and with no font nothing of the interface was drawn while the F9 window held the mouse and the keys (#26). A font
    // that still does not fit is made smaller rather than left out.
    int maxW = 2048, maxH = 2048;
    D3DCAPS9 caps{};
    if (m_dev && SUCCEEDED(m_dev->GetDeviceCaps(&caps)))
        maxW = std::clamp<int>((int)caps.MaxTextureWidth, 1024, 4096), maxH = std::clamp<int>((int)caps.MaxTextureHeight, 1024, 4096);

    HDC dc = CreateCompatibleDC(nullptr);
    auto create = [&](int height, int width) {
        return ::CreateFontW(-height, width, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
    };
    struct Pos {
        wchar_t ch;
        int x, y, cellW, adv;
    };
    std::vector<Pos> pos;
    HFONT font = nullptr;
    HGDIOBJ oldFont = nullptr;
    TEXTMETRICW tm{};
    int lineH = 0, pad = 0, texW = 1024, texH = 64, rowH = 0;
    for (;;) {
        font = create(pixelHeight, 0);
        if (stretch != 1.0f) {  // wider glyphs: an explicit average width makes GDI scale them horizontally
            HGDIOBJ prev = SelectObject(dc, font);
            TEXTMETRICW tm0;
            GetTextMetricsW(dc, &tm0);
            SelectObject(dc, prev);
            DeleteObject(font);
            font = create(pixelHeight, (int)std::lround(tm0.tmAveCharWidth * stretch));
        }
        oldFont = SelectObject(dc, font);
        GetTextMetricsW(dc, &tm);
        lineH = tm.tmHeight;
        pad = std::max(2, pixelHeight / 10);
        rowH = lineH + pad * 2;
        // pack glyphs in rows, in the narrowest atlas they fit
        bool fits = false;
        for (texW = 1024; texW <= maxW && !fits; texW *= 2) {
            pos.clear();
            pos.push_back({0, 0, 0, 4, 0});  // white block for rectangles
            int x = 4 + 2, y = 0;
            for (wchar_t ch : chars) {
                SIZE sz{};
                GetTextExtentPoint32W(dc, &ch, 1, &sz);
                int cellW = sz.cx + pad * 2;
                if (x + cellW > texW) {
                    x = 0;
                    y += rowH;
                }
                pos.push_back({ch, x, y, cellW, (int)sz.cx});
                x += cellW + 1;
            }
            for (auto& [ch, icon] : m_icons) {  // as wide as a capital is high
                int size = std::max<int>(1, tm.tmAscent - tm.tmInternalLeading), cellW = size + pad * 2;
                if (x + cellW > texW) {
                    x = 0;
                    y += rowH;
                }
                pos.push_back({ch, x, y, cellW, size});
                x += cellW + 1;
            }
            texH = 64;
            while (texH < y + rowH) texH <<= 1;
            fits = texH <= maxH;
            if (fits) break;
        }
        if (fits) break;
        SelectObject(dc, oldFont);
        DeleteObject(font);
        if (pixelHeight <= 8) {
            LOGE("Font atlas too large even at %d px", pixelHeight);
            DeleteDC(dc);
            return false;
        }
        pixelHeight = std::max(8, pixelHeight * 9 / 10);
    }
    f.tracking = tracking;
    f.upper = upper;
    f.pad = (float)pad;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = texW;
    bi.bmiHeader.biHeight = -texH;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)texW * texH * 4);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    for (auto& p : pos) {
        if (p.ch == 0 || m_icons.count(p.ch)) continue;
        TextOutW(dc, p.x + pad, p.y + pad, &p.ch, 1);
    }
    GdiFlush();
    for (auto& p : pos)  // where a capital stands: from the internal leading down to the baseline
        if (auto it = m_icons.find(p.ch); it != m_icons.end())
            DrawIcon(it->second, (uint32_t*)bits, texW, p.x + pad, p.y + pad + tm.tmInternalLeading, p.adv);

    IDirect3DTexture9* tex = nullptr;
    if (FAILED(m_dev->CreateTexture(texW, texH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) {
        LOGE("CreateTexture for font failed");
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        DeleteDC(dc);
        return false;
    }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
        const uint32_t* src = (const uint32_t*)bits;
        for (int row = 0; row < texH; ++row) {
            uint32_t* dst = (uint32_t*)((uint8_t*)lr.pBits + row * lr.Pitch);
            for (int col = 0; col < texW; ++col) {
                uint32_t px = src[row * texW + col];
                uint32_t r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
                uint32_t a = std::max(r, std::max(g, b));
                dst[col] = (a << 24) | 0x00FFFFFF;
            }
        }
        // opaque white block for rectangles
        for (int row = 0; row < 4; ++row) {
            uint32_t* dst = (uint32_t*)((uint8_t*)lr.pBits + row * lr.Pitch);
            for (int col = 0; col < 4; ++col) dst[col] = 0xFFFFFFFF;
        }
        tex->UnlockRect(0);
    }

    f.tex = tex;
    f.lineHeight = (float)lineH;
    f.pixelHeight = pixelHeight;
    f.whiteU = 2.0f / texW;
    f.whiteV = 2.0f / texH;
    f.glyphs.clear();
    for (auto& p : pos) {
        if (p.ch == 0) continue;
        Glyph g;
        g.u0 = (float)p.x / texW;
        g.v0 = (float)p.y / texH;
        g.u1 = (float)(p.x + p.cellW) / texW;
        g.v1 = (float)(p.y + rowH) / texH;
        g.w = (float)p.cellW;
        g.h = (float)rowH;
        g.advance = (float)p.adv;
        f.glyphs[p.ch] = g;
    }

    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);
    return true;
}

bool Renderer::BuildFonts() {
    Invalidate();
    int h = (int)m_height;
    int normal = std::max(12, (int)std::lround(h / 46.0));
    int small = std::max(11, (int)std::lround(h / 60.0));
    int big = std::max(16, (int)std::lround(h / 30.0));
    int menu = std::max(12, (int)std::lround(h / 45.0));
    int menuSmall = std::max(10, (int)std::lround(h / 66.0));
    // the menu font of the game is wide and spaced out: Bahnschrift stretched by a third, with tracking
    bool ok = BuildFont(m_fonts[FONT_NORMAL], L"Arial", normal, FW_BOLD) &&
              BuildFont(m_fonts[FONT_SMALL], L"Arial", small, FW_BOLD) &&
              BuildFont(m_fonts[FONT_BIG], L"Bahnschrift", big, FW_BOLD, 1.35f, big * 0.12f, true) &&
              BuildFont(m_fonts[FONT_MENU], L"Bahnschrift", menu, FW_BOLD, 1.3f, menu * 0.12f, true) &&
              BuildFont(m_fonts[FONT_MENU_SMALL], L"Bahnschrift", menuSmall, FW_SEMIBOLD, 1.3f, menuSmall * 0.16f, true);
    m_fontHeightFor = h;
    if (ok) LOGI("UI fonts created for %dx%d (%d/%d/%d/%d/%d px)", (int)m_width, h, normal, small, big, menu, menuSmall);
    return ok;
}

void Renderer::Invalidate() {
    for (auto& f : m_fonts) {
        if (f.tex) f.tex->Release();
        f.tex = nullptr;
        f.glyphs.clear();
    }
    m_ready = false;
}

bool Renderer::Begin() {
    m_dev = (IDirect3DDevice9*)game::D3DDevice();
    if (!m_dev) return false;

    D3DVIEWPORT9 vp;
    if (SUCCEEDED(m_dev->GetViewport(&vp)) && vp.Width > 0) {
        // The game may render into a viewport smaller than the back buffer (widescreen borders);
        // use the back buffer size instead.
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(m_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            D3DSURFACE_DESC d;
            bb->GetDesc(&d);
            bb->Release();
            m_width = (float)d.Width;
            m_height = (float)d.Height;
        } else {
            m_width = (float)vp.Width;
            m_height = (float)vp.Height;
        }
    }
    if (!m_ready && m_failedFor == (int)m_height) return false;  // failed for this height: not tried again every frame
    if (!m_ready || m_fontHeightFor != (int)m_height) {
        m_ready = BuildFonts();
        if (!m_ready) {
            m_failedFor = (int)m_height;
            return false;
        }
    }

    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(m_dev->CreateStateBlock(D3DSBT_ALL, &sb))) return false;
    m_stateBlock = sb;

    D3DVIEWPORT9 full{0, 0, (DWORD)m_width, (DWORD)m_height, 0.0f, 1.0f};
    m_dev->SetViewport(&full);
    m_dev->SetVertexShader(nullptr);
    m_dev->SetPixelShader(nullptr);
    m_dev->SetFVF(kFvf);
    m_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    m_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    m_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    m_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    m_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
    m_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    m_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    m_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    m_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    m_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    m_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    m_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    m_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    m_dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    m_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    m_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    m_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    m_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    m_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    m_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    m_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    m_curTex = nullptr;
    m_dev->SetTexture(0, m_fonts[FONT_NORMAL].tex);
    m_curTex = m_fonts[FONT_NORMAL].tex;
    m_verts.clear();
    m_inFrame = true;
    return true;
}

void Renderer::End() {
    if (!m_inFrame) return;
    Flush();
    auto* sb = (IDirect3DStateBlock9*)m_stateBlock;
    if (sb) {
        sb->Apply();
        sb->Release();
    }
    m_stateBlock = nullptr;
    m_inFrame = false;
}

void Renderer::Flush() {
    if (m_verts.empty() || !m_dev) return;
    m_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(m_verts.size() / 3), m_verts.data(), sizeof(Vertex));
    m_verts.clear();
}

void Renderer::SetTexture(IDirect3DTexture9* t) {
    if (t == m_curTex) return;
    Flush();
    m_dev->SetTexture(0, t);
    m_curTex = t;
}

void Renderer::Quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t c) {
    if (m_hidden) return;
    x -= 0.5f;
    y -= 0.5f;
    Vertex a{x, y, 0, 1, c, u0, v0}, b{x + w, y, 0, 1, c, u1, v0}, d{x, y + h, 0, 1, c, u0, v1},
        e{x + w, y + h, 0, 1, c, u1, v1};
    m_verts.push_back(a);
    m_verts.push_back(b);
    m_verts.push_back(d);
    m_verts.push_back(b);
    m_verts.push_back(e);
    m_verts.push_back(d);
}

void Renderer::Rect(float x, float y, float w, float h, uint32_t argb) {
    if (!m_inFrame || (argb >> 24) == 0) return;
    Font& f = m_fonts[FONT_NORMAL];
    SetTexture(f.tex);
    Quad(x, y, w, h, f.whiteU, f.whiteV, f.whiteU, f.whiteV, argb);
}

void Renderer::RectOutline(float x, float y, float w, float h, uint32_t argb, float t) {
    Rect(x, y, w, t, argb);
    Rect(x, y + h - t, w, t, argb);
    Rect(x, y + t, t, h - 2 * t, argb);
    Rect(x + w - t, y + t, t, h - 2 * t, argb);
}

std::string TalkingMark() {
    return Gfx().HasIcon(kMicIcon) ? std::string("{FFDC50}") + kMicIconUtf8 + "{FFFFFF} " : "{FFDC50}((•)){FFFFFF} ";
}

bool Renderer::SetIcon(wchar_t ch, std::string_view svg) {
    Icon icon;
    if (!ParseIcon(svg, icon)) return false;
    m_icons[ch] = std::move(icon);
    m_fontHeightFor = 0;  // the atlases are built again with it
    return true;
}

// The viewBox and every path's "d" of an SVG, as absolute moves, lines, cubic curves and closes
bool Renderer::ParseIcon(std::string_view svg, Icon& out) {
    auto space = [](char c) { return c == ' ' || c == ',' || c == '\n' || c == '\r' || c == '\t'; };
    auto attr = [&](std::string_view name, size_t from, size_t& end) -> std::string_view {
        size_t at = svg.find(name, from);
        end = at == std::string_view::npos ? at : svg.find('"', at + name.size());
        return end == std::string_view::npos ? std::string_view{} : svg.substr(at + name.size(), end - at - name.size());
    };
    auto numbers = [&](std::string_view s, size_t& i, float* to, int n) {
        for (int k = 0; k < n; ++k) {
            while (i < s.size() && space(s[i])) ++i;
            std::string num;  // strtof would read past the attribute: the number is copied out first
            while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                                    ((s[i] == '-' || s[i] == '+') && (num.empty() || num.back() == 'e' || num.back() == 'E'))))
                num += s[i++];
            if (num.empty()) return false;
            to[k] = strtof(num.c_str(), nullptr);
        }
        return true;
    };
    size_t end = 0, bi = 0;
    std::string_view box = attr("viewBox=\"", 0, end);
    float v[4];
    if (box.empty() || !numbers(box, bi, v, 4) || v[2] <= 0 || v[3] <= 0) return false;
    out.minX = v[0], out.minY = v[1], out.w = v[2], out.h = v[3];
    for (size_t from = 0;;) {
        std::string_view d = attr(" d=\"", from, end);
        if (end == std::string_view::npos) break;
        from = end;
        float cx = 0, cy = 0, sx = 0, sy = 0;  // the pen and its figure's start
        char op = 0;
        for (size_t i = 0; i < d.size();) {
            if (space(d[i])) {
                ++i;
                continue;
            }
            if (isalpha((unsigned char)d[i])) op = d[i++];
            else if (!op) return false;  // numbers before any command
            bool rel = islower((unsigned char)op);
            char up = (char)toupper((unsigned char)op);
            float p[6];
            if (up == 'M' || up == 'L') {
                if (!numbers(d, i, p, 2)) return false;
                if (rel) p[0] += cx, p[1] += cy;
                out.path.push_back({up, {p[0], p[1]}});
                cx = p[0], cy = p[1];
                if (up == 'M') sx = cx, sy = cy, op = rel ? 'l' : 'L';  // its next pairs are lines
            } else if (up == 'H' || up == 'V') {
                if (!numbers(d, i, p, 1)) return false;
                float& c = up == 'H' ? cx : cy;
                c = p[0] + (rel ? c : 0.0f);
                out.path.push_back({'L', {cx, cy}});
            } else if (up == 'C') {
                if (!numbers(d, i, p, 6)) return false;
                if (rel)
                    for (int k = 0; k < 6; k += 2) p[k] += cx, p[k + 1] += cy;
                out.path.push_back({'C', {p[0], p[1], p[2], p[3], p[4], p[5]}});
                cx = p[4], cy = p[5];
            } else if (up == 'Z') {
                out.path.push_back({'Z', {}});
                cx = sx, cy = sy;
                op = 0;
            } else {
                return false;  // S, Q, T, A: not read
            }
        }
    }
    return !out.path.empty();
}

// The icon filled white into the atlas (its square of size x size at x0, y0): drawn 4 times bigger by GDI, which has no
// smoothing of its own, and averaged down
void Renderer::DrawIcon(const Icon& icon, uint32_t* bits, int texW, int x0, int y0, int size) {
    constexpr int kS = 4;
    int big = size * kS;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = big;
    bi.bmiHeader.biHeight = -big;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    void* px = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &px, nullptr, 0);
    if (!bmp) {
        DeleteDC(dc);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(px, 0, (size_t)big * big * 4);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(WHITE_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    SetPolyFillMode(dc, WINDING);  // an SVG's nonzero rule
    float k = big / std::max(icon.w, icon.h), ox = (big - icon.w * k) / 2, oy = (big - icon.h * k) / 2;
    auto at = [&](float x, float y) {
        return POINT{(LONG)std::lround(ox + (x - icon.minX) * k), (LONG)std::lround(oy + (y - icon.minY) * k)};
    };
    BeginPath(dc);
    for (const PathStep& s : icon.path) {
        if (s.op == 'M') {
            POINT a = at(s.p[0], s.p[1]);
            MoveToEx(dc, a.x, a.y, nullptr);
        } else if (s.op == 'L') {
            POINT a = at(s.p[0], s.p[1]);
            LineTo(dc, a.x, a.y);
        } else if (s.op == 'C') {
            POINT c[3] = {at(s.p[0], s.p[1]), at(s.p[2], s.p[3]), at(s.p[4], s.p[5])};
            PolyBezierTo(dc, c, 3);
        } else {
            CloseFigure(dc);
        }
    }
    EndPath(dc);
    FillPath(dc);
    GdiFlush();
    const uint32_t* src = (const uint32_t*)px;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            uint32_t sum = 0;
            for (int j = 0; j < kS; ++j)
                for (int i = 0; i < kS; ++i) sum += (src[(y * kS + j) * big + x * kS + i] >> 16) & 0xFF;
            uint32_t g = sum / (kS * kS);
            bits[(y0 + y) * texW + x0 + x] = (g << 16) | (g << 8) | g;  // white of this strength: its alpha in the atlas
        }
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

wchar_t Renderer::Shape(const Font& f, wchar_t c) {
    return f.upper ? (wchar_t)(uintptr_t)CharUpperW((LPWSTR)(uintptr_t)c) : c;  // one character in the low word
}

const Renderer::Glyph* Renderer::GetGlyph(Font& f, wchar_t ch) {
    auto it = f.glyphs.find(ch);
    if (it != f.glyphs.end()) return &it->second;
    if (ch == 0x401) return GetGlyph(f, 0x415);  // Ё -> Е if missing
    it = f.glyphs.find(L'?');
    return it != f.glyphs.end() ? &it->second : nullptr;
}

static bool ParseColorCode(std::wstring_view t, size_t i, uint32_t& rgb) {
    if (i + 7 >= t.size() + 0 || t[i] != L'{' || i + 7 >= t.size() || t[i + 7] != L'}') return false;
    uint32_t v = 0;
    for (size_t k = 1; k <= 6; ++k) {
        wchar_t c = t[i + k];
        v <<= 4;
        if (c >= L'0' && c <= L'9') v |= c - L'0';
        else if (c >= L'a' && c <= L'f') v |= c - L'a' + 10;
        else if (c >= L'A' && c <= L'F') v |= c - L'A' + 10;
        else return false;
    }
    rgb = v;
    return true;
}

void Renderer::DrawRun(Font& f, float x, float y, std::wstring_view text, uint32_t argb) {
    SetTexture(f.tex);
    float pen = x;
    uint32_t color = argb;
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t rgb;
        if (text[i] == L'{' && ParseColorCode(text, i, rgb)) {
            color = (argb & 0xFF000000) | rgb;
            i += 7;
            continue;
        }
        const Glyph* g = GetGlyph(f, Shape(f, text[i]));
        if (!g) continue;
        if (text[i] != L' ') Quad(std::floor(pen - f.pad), std::floor(y - f.pad), g->w, g->h, g->u0, g->v0, g->u1, g->v1, color);
        pen += g->advance + f.tracking;
    }
}

float Renderer::TextWidthW(FontId fid, std::wstring_view text) {
    Font& f = m_fonts[fid];
    float w = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t rgb;
        if (text[i] == L'{' && ParseColorCode(text, i, rgb)) {
            i += 7;
            continue;
        }
        if (const Glyph* g = GetGlyph(f, Shape(f, text[i]))) w += g->advance + f.tracking;
    }
    return w;
}

float Renderer::TextWidth(FontId f, std::string_view utf8) { return TextWidthW(f, Widen(utf8)); }

void Renderer::TextW(FontId fid, float x, float y, std::wstring_view text, uint32_t argb, uint32_t flags) {
    if (!m_inFrame || text.empty()) return;
    Font& f = m_fonts[fid];
    if (flags & (TEXT_CENTER | TEXT_RIGHT)) {
        float w = TextWidthW(fid, text);
        x -= (flags & TEXT_CENTER) ? w * 0.5f : w;
    }
    x = std::floor(x);
    y = std::floor(y);
    uint32_t shadow = WithAlpha(0xFF000000, (float)(argb >> 24) / 255.0f * 0.85f);
    // shadows must not take inline colours: strip codes by drawing with a fixed colour
    auto drawShadow = [&](float dx, float dy) {
        std::wstring plain;
        plain.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            uint32_t rgb;
            if (text[i] == L'{' && ParseColorCode(text, i, rgb)) {
                i += 7;
                continue;
            }
            plain += text[i];
        }
        DrawRun(f, x + dx, y + dy, plain, shadow);
    };
    if (flags & TEXT_OUTLINE) {
        drawShadow(-1, 0);
        drawShadow(1, 0);
        drawShadow(0, -1);
        drawShadow(0, 1);
    } else if (flags & TEXT_SHADOW) {
        drawShadow(1, 1);
    }
    DrawRun(f, x, y, text, argb);
}

void Renderer::Text(FontId f, float x, float y, std::string_view utf8, uint32_t argb, uint32_t flags) {
    TextW(f, x, y, Widen(utf8), argb, flags);
}

std::vector<std::wstring> Renderer::Wrap(FontId fid, std::wstring_view text, float maxWidth) {
    std::vector<std::wstring> lines;
    std::wstring cur;
    std::wstring lastColor;
    float curW = 0;
    Font& f = m_fonts[fid];
    size_t lastSpace = std::wstring::npos;
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t rgb;
        if (text[i] == L'{' && ParseColorCode(text, i, rgb)) {
            lastColor = std::wstring(text.substr(i, 8));
            cur += lastColor;
            i += 7;
            continue;
        }
        wchar_t ch = text[i];
        const Glyph* g = GetGlyph(f, Shape(f, ch));
        float adv = g ? g->advance + f.tracking : 0;
        if (curW + adv > maxWidth && !cur.empty()) {
            if (lastSpace != std::wstring::npos && lastSpace > 0) {
                std::wstring rest = cur.substr(lastSpace + 1);
                cur.resize(lastSpace);
                lines.push_back(cur);
                cur = lastColor + rest;
                curW = TextWidthW(fid, rest);
            } else {
                lines.push_back(cur);
                cur = lastColor;
                curW = 0;
            }
            lastSpace = std::wstring::npos;
        }
        if (ch == L' ') lastSpace = cur.size();
        cur += ch;
        curW += adv;
    }
    if (!cur.empty()) lines.push_back(cur);
    if (lines.empty()) lines.emplace_back();
    return lines;
}

bool Renderer::SaveFrame(const std::string& bmpPath, int downscale) {
    auto* dev = (IDirect3DDevice9*)game::D3DDevice();
    if (!dev || downscale < 1) return false;
    IDirect3DSurface9 *bb = nullptr, *plain = nullptr, *sys = nullptr;
    bool ok = false;
    D3DSURFACE_DESC d;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && SUCCEEDED(bb->GetDesc(&d)) &&
        SUCCEEDED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &plain, nullptr)) &&
        SUCCEEDED(dev->StretchRect(bb, nullptr, plain, nullptr, D3DTEXF_NONE)) &&  // resolves a multisampled buffer
        SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(plain, sys))) {
        D3DLOCKED_RECT lr;
        if ((d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            int w = (int)d.Width / downscale, h = (int)d.Height / downscale, stride = (w * 3 + 3) & ~3;
            std::vector<uint8_t> file(54 + (size_t)stride * h);
            BITMAPFILEHEADER* fh = (BITMAPFILEHEADER*)file.data();
            BITMAPINFOHEADER* ih = (BITMAPINFOHEADER*)(file.data() + 14);
            fh->bfType = 0x4D42;
            fh->bfSize = (DWORD)file.size();
            fh->bfOffBits = 54;
            ih->biSize = 40;
            ih->biWidth = w;
            ih->biHeight = h;  // bottom-up
            ih->biPlanes = 1;
            ih->biBitCount = 24;
            for (int y = 0; y < h; ++y) {
                const uint8_t* src = (const uint8_t*)lr.pBits + (size_t)(y * downscale) * lr.Pitch;
                uint8_t* dst = file.data() + 54 + (size_t)(h - 1 - y) * stride;
                for (int x = 0; x < w; ++x) memcpy(dst + x * 3, src + (size_t)x * downscale * 4, 3);  // B, G, R
            }
            sys->UnlockRect();
            if (FILE* f = fopen(bmpPath.c_str(), "wb")) {
                ok = fwrite(file.data(), 1, file.size(), f) == file.size();
                fclose(f);
            }
        }
    }
    if (sys) sys->Release();
    if (plain) plain->Release();
    if (bb) bb->Release();
    return ok;
}

// ---------------------------------------------------------------- the game's own fonts
namespace {
// CFont keeps its settings in statics (colour, scale, alignment, wrap, style, outline, render state): they are
// saved and restored around our text so the game's own printing is not affected.
constexpr uintptr_t kFontStatics = 0xC71A60;
constexpr size_t kFontStaticsSize = 0x70;
constexpr float kCellAspect = 0.55f;  // glyph cell width : height, close to the game's menu titles

uint32_t ToRgba(uint32_t argb) {  // CRGBA in memory: r, g, b, a
    return ((argb >> 16) & 0xFF) | (argb & 0xFF00) | ((argb & 0xFF) << 16) | (argb & 0xFF000000);
}

void SetupGameFont(GameFont font, float height, uint32_t argb) {
    float sy = height / 16.0f;  // CFont::PrintChar: cell height = 16 * scale.y
    game::Call<0x719490>((short)font);                                          // SetFontStyle
    game::Call<0x719380>(sy * kCellAspect, sy);                                  // SetScale
    game::Call<0x719430>(ToRgba(argb));                                         // SetColor
    game::Call<0x719510>(ToRgba(argb & 0xFF000000));                            // SetDropColor
    game::Call<0x719590>((short)std::max(1, (int)std::lround(height / 28.0f)));  // SetOutlinePosition
    game::Call<0x7195B0>(true);                                                 // SetProp
    game::Call<0x7195C0>(false, false);                                         // SetBackground
    game::Call<0x719610>(1);                                                    // SetAlignment(ALIGN_LEFT)
    game::Call<0x7194D0>(100000.0f);                                            // SetWrapx: no wrapping
    game::Call<0x719420>(0.0f);                                                 // SetSlant
    game::Call<0x719500>(255.0f);                                               // SetAlphaFade
}
}  // namespace

float Renderer::GameTextWidth(GameFont font, std::string_view ascii, float height) {
    std::string s(ascii);
    uint8_t saved[kFontStaticsSize];
    memcpy(saved, (void*)kFontStatics, sizeof(saved));
    SetupGameFont(font, height, 0xFFFFFFFF);
    float w = game::Call<0x71A0E0, float>(s.data(), true, false);  // CFont::GetStringWidth
    memcpy((void*)kFontStatics, saved, sizeof(saved));
    return w;
}

void Renderer::GameText(GameFont font, float x, float y, std::string_view ascii, float height, uint32_t argb,
                        uint32_t flags) {
    if (!m_inFrame || m_hidden || ascii.empty()) return;
    if (flags & (TEXT_CENTER | TEXT_RIGHT)) {
        float w = GameTextWidth(font, ascii, height);
        x -= (flags & TEXT_CENTER) ? w * 0.5f : w;
    }
    std::string s(ascii);  // CFont wants a writable, NUL-terminated string
    End();                 // the game draws with its own device state
    uint8_t saved[kFontStaticsSize];
    memcpy(saved, (void*)kFontStatics, sizeof(saved));
    SetupGameFont(font, height, argb);
    game::Call<0x71A700>(x, y, (unsigned char*)s.data());  // CFont::PrintString
    game::Call<0x71A210>();                                 // CFont::DrawFonts
    memcpy((void*)kFontStatics, saved, sizeof(saved));
    Begin();
}

}  // namespace coop::ui
