// The U window (AnimsMenu.h). The emotes, what they do and the preview's picture are the feature's (features/anims).
#include "pages/anims-menu/AnimsMenu.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#include "common/lib/util/Util.h"
#include "features/anims/Emotes.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/input/Input.h"
#include "shared/ui/renderer/Renderer.h"
#include "shared/ui/widgets/Widgets.h"

namespace coop::ui {

namespace {
using anims::Emote;
using w::kSaBlue, w::kSaMuted, w::kSaRed, w::kSaLine;
constexpr uint32_t kText = 0xFFE6E9EF;

bool g_visible = false;
bool g_captured = false;  // the keys and the mouse taken (from the first frame drawn, see SetAnimsMenuVisible)
uint64_t g_openedMs = 0;
anims::Kind g_kind = anims::EMOTION;
int g_sel[anims::KINDS] = {};  // each tab's chosen row
int g_scroll = 0;
float g_view = 0.0f;      // the row at the list's top on screen: it glides to g_scroll, a notch of the wheel jumped rows
uint64_t g_viewMs = 0;
bool g_reveal = false;   // the chosen row brought into sight on the next frame (set from outside the window's keys)
uint16_t g_clickId = 0;  // a second click on a row soon after plays it
uint64_t g_clickMs = 0;
float g_dragX = -1.0f;  // the preview turned by dragging it
std::string g_note;
bool g_noteError = false;
uint64_t g_noteUntil = 0;

void Note(const std::string& text, bool error) { g_note = text, g_noteError = error, g_noteUntil = NowMs() + 6000; }

// a letter in capitals, Cyrillic too (CharUpperW takes one character in the pointer's low word)
wchar_t Upper(wchar_t c) { return (wchar_t)(uintptr_t)CharUpperW((LPWSTR)(uintptr_t)c); }

bool Hovered(float x, float y, float w, float h) {
    auto& st = In().State();
    return st.mouseX >= x && st.mouseX < x + w && st.mouseY >= y && st.mouseY < y + h;
}

std::vector<const Emote*> Items(anims::Kind k) {
    std::vector<const Emote*> v;
    for (const Emote& e : anims::List())
        if (e.kind == k) v.push_back(&e);
    return v;
}

// the text cut to the width with an ellipsis (a long name of a player's own file)
std::string Fit(FontId f, std::string text, float width) {
    auto& g = Gfx();
    if (g.TextWidth(f, text) <= width) return text;
    std::wstring w = Widen(text);
    while (!w.empty() && g.TextWidthW(f, w + L"…") > width) w.pop_back();
    return Narrow(w + L"…");
}

void PlayChosen(const Emote* e) {
    std::string why;
    if (!e) return;
    if (anims::Play(*e, why)) SetAnimsMenuVisible(false);
    else Note(why, true);
}
}  // namespace

void SetAnimsMenuVisible(bool visible) {
    if (visible == g_visible) return;
    g_visible = visible;
    g_openedMs = visible ? NowMs() : 0;
    g_noteUntil = 0;
    g_dragX = -1.0f;
    if (visible) {
        if (TheChat().IsOpen()) TheChat().Close();
        return;  // the keys are taken from its first frame on: the U that opened it still reaches the game (a cheat code)
    }
    anims::PreviewHide();
    if (!std::exchange(g_captured, false) || core::ConnectMenuVisible()) return;  // F9 opened over it keeps them
    In().SetKeyboardCapture(false);
    In().SetMouseCapture(false);
}

bool IsAnimsMenuVisible() { return g_visible; }

uint64_t AnimsMenuOpenedMs() { return g_openedMs; }

void ToggleAnimsMenu() {
    if (g_visible) return SetAnimsMenuVisible(false);
    if (!core::IsPlayerReady() || game::MenuActive() || core::ConnectMenuVisible() || Gfx().Failed()) return;
    // in a car its keys would stop the driving, and in the rest it could not play one anyway
    if (std::string why = anims::WhyNot(); !why.empty()) return TheChat().AddError(why);
    SetAnimsMenuVisible(true);
}

void TestAnimsMenu(uint16_t emote) {
    SetAnimsMenuVisible(true);
    const Emote* e = anims::Find(emote);
    if (!e) return;
    g_kind = e->kind;
    auto items = Items(g_kind);
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i]->id == emote) g_sel[g_kind] = (int)i, g_scroll = 0, g_view = 0.0f, g_reveal = true;
}

void RenderAnimsMenu() {
    if (!g_visible) return;
    if (!core::IsPlayerReady() || game::MenuActive() || core::ConnectMenuVisible() || game::Ref<uint8_t>(0xB5F851))  // a cutscene
        return SetAnimsMenuVisible(false);
    bool first = !g_captured;  // the keys of this frame are older than the window: the Enter of the chat's /anims
    if (first) {
        In().SetKeyboardCapture(true);
        In().SetMouseCapture(true);
        g_captured = true;
    }
    auto& g = Gfx();
    auto& st = In().State();
    float s = g.Scale();
    float lh = g.LineHeight(FONT_NORMAL), sh = g.LineHeight(FONT_SMALL), mh = g.LineHeight(FONT_MENU), ch = g.LineHeight(FONT_MENU_SMALL);
    float pad = 28 * s, room = mh * 1.6f;  // room: the space beside a button's name
    float W = std::min(1180 * s, g.Width() - 32 * s), H = std::min(720 * s, g.Height() - 32 * s);
    float x = (g.Width() - W) * 0.5f, y = std::max(16 * s, (g.Height() - H) * 0.45f);
    float cx = x + pad, cy = y + 22 * s, cw = W - 2 * pad;
    w::BeginFrame();
    g.Rect(x, y, W, H, 0xF0000000);
    g.RectOutline(x, y, W, H, kSaLine, std::max(1.0f, s));
    g.Text(FONT_MENU, cx, cy, TR("Анимации", "Animations"), kSaBlue, TEXT_SHADOW);
    cy += mh + 14 * s;

    // the keys: up and down the list, left and right between the tabs, Enter plays (U and Esc close it, Core), Page Up and
    // Down, Home and End, and a letter typed (in the keyboard's language) goes to the next pack whose title starts with it
    bool play = false, moved = false, home = false, end = false;
    int pages = 0;
    wchar_t letter = 0;
    if (!first) {
        for (int vk : st.keyDowns) {
            if (vk == VK_UP || vk == VK_DOWN) g_sel[g_kind] += vk == VK_UP ? -1 : 1, moved = true;
            if (vk == VK_LEFT || vk == VK_RIGHT)
                g_kind = (anims::Kind)((g_kind + (vk == VK_LEFT ? anims::KINDS - 1 : 1)) % anims::KINDS), g_scroll = 0, g_view = 0.0f;
            if (vk == VK_RETURN) play = true;
            if (vk == VK_PRIOR || vk == VK_NEXT) pages += vk == VK_PRIOR ? -1 : 1;
            if (vk == VK_HOME) home = true;
            if (vk == VK_END) end = true;
        }
        for (wchar_t c : st.chars)
            if (IsCharAlphaW(c)) letter = Upper(c);
    }

    // the tabs share the width: the space round their names shrinks to fit (the English ones are longer)
    float names = 0.0f;
    for (int k = 0; k < anims::KINDS; ++k) names += g.TextWidth(FONT_MENU_SMALL, anims::KindName((anims::Kind)k));
    float space = std::clamp((cw - names) / (float)anims::KINDS, 8 * s, 40 * s), tx = cx;
    for (int k = 0; k < anims::KINDS; ++k) {
        const char* label = anims::KindName((anims::Kind)k);
        float tw = g.TextWidth(FONT_MENU_SMALL, label) + space;
        if (w::Tab("anim-tab", tx, cy, tw, ch + 12 * s, label, g_kind == k, true)) g_kind = (anims::Kind)k, g_scroll = 0, g_view = 0.0f;
        tx += tw;
    }
    cy += ch + 26 * s;

    float rowH = lh + 12 * s;
    float footY = y + H - 18 * s - rowH;  // the buttons
    float bodyH = footY - sh - 22 * s - cy;
    float pv = std::min(bodyH, cw * 0.45f);  // the preview's square
    float listW = cw - pv - 24 * s;

    // the list; in the packs' tab each pack's rows go under its title. A row is an item's index, or -1 - i for the
    // title over item i
    auto items = Items(g_kind);
    std::vector<int> rows;
    for (int i = 0; i < (int)items.size(); ++i) {
        if (!items[i]->pack.empty() && (i == 0 || items[i]->pack != items[i - 1]->pack)) rows.push_back(-1 - i);
        rows.push_back(i);
    }
    int n = (int)items.size(), nr = (int)rows.size(), shown = std::max(1, (int)(bodyH / rowH));
    int& sel = g_sel[g_kind];
    if (pages) sel += pages * std::max(1, shown - 1), moved = true;
    if (home) sel = 0, moved = true;
    if (end) sel = n - 1, moved = true;
    sel = n ? std::clamp(sel, 0, n - 1) : 0;
    for (int k = 1; letter && k <= n; ++k) {  // the next block of that letter, round to the top
        int i = (sel + k) % n;
        const std::string& p = items[i]->pack;
        if (!p.empty() && (i == 0 || items[i - 1]->pack != p) && Upper(Widen(p)[0]) == letter) {
            sel = i, moved = true;
            break;
        }
    }
    int selRow = (int)(std::find(rows.begin(), rows.end(), sel) - rows.begin());
    float listH = (float)shown * rowH - 3 * s;
    if (st.wheel && Hovered(cx, cy, listW, listH)) g_scroll -= st.wheel * 3;
    if (moved || std::exchange(g_reveal, false)) {  // the chosen row in sight, with its pack's title when it is the first
        int top = selRow > 0 && rows[selRow - 1] < 0 ? selRow - 1 : selRow, low = selRow - shown + 1;
        g_scroll = std::clamp(g_scroll, low, std::max(low, top));
    }
    g_scroll = std::clamp(g_scroll, 0, std::max(0, nr - shown));
    uint64_t nowMs = NowMs();
    g_view += ((float)g_scroll - g_view) * (1.0f - std::exp(-(float)std::min<uint64_t>(nowMs - g_viewMs, 100) / 60.0f));
    g_viewMs = nowMs;
    if (std::fabs((float)g_scroll - g_view) < 0.01f) g_view = (float)g_scroll;
    const Emote* chosen = n ? items[sel] : nullptr;
    g.Clip(cx, cy, listW, listH);
    for (int r = (int)g_view; r < std::min(nr, (int)g_view + shown + 1); ++r) {
        float ry = cy + ((float)r - g_view) * rowH;
        if (rows[r] < 0) {
            g.Text(FONT_SMALL, cx + 10 * s, ry + (rowH - sh) * 0.5f, Fit(FONT_SMALL, items[-1 - rows[r]]->pack, listW - 20 * s),
                   kSaMuted, TEXT_SHADOW);
            continue;
        }
        int i = rows[r];
        bool hover = Hovered(cx, ry, listW, rowH) && Hovered(cx, cy, listW, listH);
        g.Rect(cx, ry, listW, rowH - 3 * s, i == sel ? 0xFF16202A : hover ? 0xFF10161D : 0xFF090C10);
        g.Text(FONT_SMALL, cx + 58 * s, ry + (rowH - sh) * 0.5f, std::to_string(items[i]->id), kSaMuted, TEXT_RIGHT);
        g.Text(FONT_NORMAL, cx + 74 * s, ry + (rowH - lh) * 0.5f, Fit(FONT_NORMAL, items[i]->name, listW - 90 * s),
               i == sel ? kSaBlue : kText, TEXT_SHADOW);
        if (hover && st.lmbPressed) {
            if (i == sel && g_clickId == items[i]->id && NowMs() - g_clickMs < 450) play = true;
            sel = i, chosen = items[i], g_clickId = items[i]->id, g_clickMs = NowMs();
        }
    }
    g.ClipEnd();
    if (nr > shown) {  // where the list is scrolled to
        float bh = listH * (float)shown / (float)nr, by = cy + listH * g_view / (float)nr;
        g.Rect(cx + listW - 4 * s, by, 3 * s, bh, kSaMuted);
    }
    if (!n && g_kind == anims::PACKS)
        for (auto& line : g.Wrap(FONT_SMALL,
                                 Widen(Format(TR("В папке %s нет паков анимаций.", "There are no animation packs in the %s folder."),
                                              anims::PacksFolder().c_str())),
                                 listW))
            g.TextW(FONT_SMALL, cx, cy, line, kSaMuted, TEXT_SHADOW), cy += sh;
    if (!n && g_kind == anims::CUSTOM)
        for (auto& line : g.Wrap(FONT_SMALL,
                                 Widen(Format(TR("Свои анимации берутся из файлов .ifp в папке %s. Игра читает их при запуске.",
                                                 "Own animations come from .ifp files in the %s folder. The game reads them when it starts."),
                                              anims::CustomFolder().c_str())),
                                 listW))
            g.TextW(FONT_SMALL, cx, cy, line, kSaMuted, TEXT_SHADOW), cy += sh;

    // the preview: a copy of CJ playing the chosen one, turned by dragging it
    float px = cx + listW + 24 * s, py = y + mh + ch + 62 * s;
    g.Rect(px, py, pv, pv, 0xFF0C1117);
    if (void* tex = anims::PreviewTexture()) g.Image(tex, px, py, pv, pv);
    g.RectOutline(px, py, pv, pv, kSaLine, std::max(1.0f, s));
    if (st.lmb && (g_dragX >= 0.0f || (st.lmbPressed && Hovered(px, py, pv, pv)))) {
        if (g_dragX >= 0.0f) anims::PreviewTurn((st.mouseX - g_dragX) * 0.01f);
        g_dragX = st.mouseX;
    } else {
        g_dragX = -1.0f;
    }
    anims::PreviewShow(chosen, pv);

    // under it: what plays it from the chat, or what the last try said
    float ny = footY - sh - 12 * s;
    if (NowMs() < g_noteUntil) g.Text(FONT_SMALL, cx, ny, Fit(FONT_SMALL, g_note, cw), g_noteError ? kSaRed : kSaBlue, TEXT_SHADOW);
    else if (chosen) g.Text(FONT_SMALL, cx, ny, Format(TR("В чате: /anims %u", "In the chat: /anims %u"), chosen->id), kSaMuted, TEXT_SHADOW);
    const char* playText = TR("Включить", "Play");
    const char* close = TR("Закрыть", "Close");
    float closeX = cx + cw - g.TextWidth(FONT_MENU, close), playX = closeX - room - 40 * s - g.TextWidth(FONT_MENU, playText);
    if (w::Button("anim-play", playX - room, footY, g.TextWidth(FONT_MENU, playText) + 2 * room, rowH, playText, chosen != nullptr)) play = true;
    bool closing = w::Button("anim-close", closeX - room, footY, g.TextWidth(FONT_MENU, close) + 2 * room, rowH, close);
    w::EndFrame();
    w::DrawCursor();
    if (play) PlayChosen(chosen);
    else if (closing) SetAnimsMenuVisible(false);
}

}  // namespace coop::ui
