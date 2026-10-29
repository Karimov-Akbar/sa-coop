#pragma once
// Minimal immediate-mode 2D renderer on top of the game's IDirect3DDevice9.
// Text uses glyph atlases rendered with GDI, so any Windows font with full Cyrillic support works.
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace coop::ui {

// NORMAL/SMALL: Arial Bold (SA subtitles / SA-MP chat look); BIG/MENU/MENU_SMALL: wide capitals with tracking,
// close to the game's menu font.
enum FontId { FONT_NORMAL = 0, FONT_SMALL = 1, FONT_BIG = 2, FONT_MENU = 3, FONT_MENU_SMALL = 4, FONT_COUNT };

// The game's own fonts (CFont styles).
enum GameFont { GAME_FONT_GOTHIC = 0, GAME_FONT_SUBTITLES = 1, GAME_FONT_MENU = 2, GAME_FONT_PRICEDOWN = 3 };

enum TextFlags : uint32_t {
    TEXT_SHADOW = 1,
    TEXT_OUTLINE = 2,
    TEXT_CENTER = 4,   // x is the centre
    TEXT_RIGHT = 8,    // x is the right edge
};

// ARGB helpers
constexpr uint32_t Argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
inline uint32_t RgbaToArgb(uint32_t rgba) { return (rgba >> 8) | ((rgba & 0xFF) << 24); }
inline uint32_t WithAlpha(uint32_t argb, float a) {
    uint32_t al = (uint32_t)((float)(argb >> 24) * (a < 0 ? 0 : a > 1 ? 1 : a));
    return (argb & 0x00FFFFFF) | (al << 24);
}

class Renderer {
public:
    // Must be called inside the game's frame (between BeginScene/EndScene).
    bool Begin();
    void End();

    void Rect(float x, float y, float w, float h, uint32_t argb);
    void RectOutline(float x, float y, float w, float h, uint32_t argb, float t = 1.0f);
    // Text in UTF-8. Supports inline colour codes {RRGGBB}.
    void Text(FontId f, float x, float y, std::string_view utf8, uint32_t argb, uint32_t flags = TEXT_SHADOW);
    void TextW(FontId f, float x, float y, std::wstring_view text, uint32_t argb, uint32_t flags = TEXT_SHADOW);
    float TextWidth(FontId f, std::string_view utf8);
    float TextWidthW(FontId f, std::wstring_view text);
    float LineHeight(FontId f) const { return m_fonts[f].lineHeight; }
    // Splits text into lines not wider than maxWidth (colour codes are kept).
    std::vector<std::wstring> Wrap(FontId f, std::wstring_view text, float maxWidth);
    // Text in one of the game's fonts, drawn by the game (CFont), black outline. ASCII only: the glyphs above it
    // differ between translations. height = character cell height in pixels.
    void GameText(GameFont font, float x, float y, std::string_view ascii, float height, uint32_t argb, uint32_t flags = 0);
    float GameTextWidth(GameFont font, std::string_view ascii, float height);
    // A picture drawn as one more character of the fonts (a private-use code point, U+E000 and on) from an SVG's path:
    // it takes the text's colour as a letter does and stands as high as a capital. False: no path it can read (the
    // commands M L H V C Z only).
    bool SetIcon(wchar_t ch, std::string_view svg);
    bool HasIcon(wchar_t ch) const { return m_icons.count(ch) != 0; }

    float Width() const { return m_width; }
    float Height() const { return m_height; }
    // UI scale relative to 1080p.
    float Scale() const { return m_height / 1080.0f; }
    bool Ready() const { return m_ready; }
    // the fonts could not be built for this screen: nothing of the interface can be drawn
    bool Failed() const { return !m_ready && m_failedFor != 0 && m_failedFor == (int)m_height; }
    // nothing drawn shows (the frame's callers still run): a clean picture to record
    void SetHidden(bool hidden) { m_hidden = hidden; }

    void Invalidate();  // destroy textures (device reset / shutdown)
    // Tests: the frame drawn so far (the game's back buffer, never other windows) as a 24-bit BMP, scaled down.
    bool SaveFrame(const std::string& bmpPath, int downscale);

private:
    struct Glyph {
        float u0, v0, u1, v1;
        float w, h;      // quad size in pixels
        float advance;   // pen advance
    };
    struct Font {
        IDirect3DTexture9* tex = nullptr;
        std::unordered_map<wchar_t, Glyph> glyphs;
        float lineHeight = 0;
        float pad = 2;
        float whiteU = 0, whiteV = 0;  // a fully opaque texel (used for rectangles)
        int pixelHeight = 0;
        float tracking = 0;  // extra space after every glyph
        bool upper = false;  // drawn in capitals
    };
    struct Vertex {
        float x, y, z, rhw;
        uint32_t color;
        float u, v;
    };
    struct PathStep {
        char op;  // M, L, C (two control points and the end), Z
        float p[6];
    };
    struct Icon {
        float minX = 0, minY = 0, w = 0, h = 0;  // the SVG's viewBox
        std::vector<PathStep> path;              // absolute
    };

    bool BuildFonts();
    bool BuildFont(Font& f, const wchar_t* face, int pixelHeight, int weight, float stretch = 1.0f, float tracking = 0,
                   bool upper = false);
    static wchar_t Shape(const Font& f, wchar_t c);
    static bool ParseIcon(std::string_view svg, Icon& out);
    static void DrawIcon(const Icon& icon, uint32_t* bits, int texW, int x0, int y0, int size);
    void Flush();
    void SetTexture(IDirect3DTexture9* t);
    void Quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t c);
    const Glyph* GetGlyph(Font& f, wchar_t ch);
    void DrawRun(Font& f, float x, float y, std::wstring_view text, uint32_t argb);

    IDirect3DDevice9* m_dev = nullptr;
    void* m_stateBlock = nullptr;
    Font m_fonts[FONT_COUNT];
    std::unordered_map<wchar_t, Icon> m_icons;
    IDirect3DTexture9* m_curTex = nullptr;
    std::vector<Vertex> m_verts;
    float m_width = 1920, m_height = 1080;
    int m_fontHeightFor = 0;  // screen height the atlases were built for
    int m_failedFor = 0;      // screen height they could not be built for
    bool m_ready = false;
    bool m_inFrame = false;
    bool m_hidden = false;
};

Renderer& Gfx();

// The microphone's picture among the fonts' characters (Voice reads it from ui/microphone.svg), and in UTF-8
constexpr wchar_t kMicIcon = 0xE000;
constexpr const char* kMicIconUtf8 = "\xEE\x80\x80";
// "talking" before a name: the microphone in yellow ("((•))" while the picture is not in the fonts)
std::string TalkingMark();

// UTF-8 <-> wide helpers used by the UI
std::wstring Widen(std::string_view s);
std::string Narrow(std::wstring_view s);

}  // namespace coop::ui
