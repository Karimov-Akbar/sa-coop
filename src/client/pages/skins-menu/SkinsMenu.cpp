// The F4 window (SkinsMenu.h). The list, wearing one and the preview's picture are the features' (features/skins,
// features/anims), the look is the U window's.
#include "pages/skins-menu/SkinsMenu.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "common/lib/util/Util.h"
#include "features/anims/Emotes.h"
#include "features/skins/Skins.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/input/Input.h"
#include "shared/ui/renderer/Renderer.h"
#include "shared/ui/widgets/Widgets.h"

namespace coop::ui {

namespace {
using skins::Skin;
using w::kSaBlue, w::kSaMuted, w::kSaRed, w::kSaLine;
constexpr uint32_t kText = 0xFFE6E9EF;

bool g_visible = false;
bool g_captured = false;
skins::Group g_group = skins::MEN;
int g_sel[skins::GROUPS] = {};
int g_scroll = 0;
float g_view = 0.0f;
uint64_t g_viewMs = 0;
bool g_reveal = false;
int g_clickRow = -1;  // a double click: the same row of the same tab
uint64_t g_clickMs = 0;
float g_dragX = -1.0f;
std::string g_note;
bool g_noteError = false;
uint64_t g_noteUntil = 0;

void Note(const std::string& text, bool error) { g_note = text, g_noteError = error, g_noteUntil = NowMs() + 6000; }

// the text cut to the width with an ellipsis (a long title of a pack)
std::string Fit(FontId f, std::string text, float width) {
    auto& g = Gfx();
    if (g.TextWidth(f, text) <= width) return text;
    std::wstring w = Widen(text);
    while (!w.empty() && g.TextWidthW(f, w + L"\u2026") > width) w.pop_back();
    return Narrow(w + L"\u2026");
}

bool Hovered(float x, float y, float w, float h) {
    auto& st = In().State();
    return st.mouseX >= x && st.mouseX < x + w && st.mouseY >= y && st.mouseY < y + h;
}

std::vector<const Skin*> Items(skins::Group g) {
    std::vector<const Skin*> v;
    for (const Skin& s : skins::List())
        if (s.group == g) v.push_back(&s);
    return v;
}

void WearChosen(const Skin* s) {
    std::string why;
    if (skins::Wear(s, why)) Note(s ? Format(TR("Надет скин %s.", "Skin %s is on."), s->name.c_str()) : TR("Вы снова CJ.", "You are CJ again."), false);
    else Note(why, true);
}
}  // namespace

void SetSkinsMenuVisible(bool visible) {
    if (visible == g_visible) return;
    g_visible = visible;
    g_noteUntil = 0;
    g_dragX = -1.0f;
    if (visible) {
        if (TheChat().IsOpen()) TheChat().Close();
        skins::Rescan();  // files put into coop\skins meanwhile
        // the worn one chosen in its tab
        for (int k = 0; k < skins::GROUPS; ++k) {
            auto items = Items((skins::Group)k);
            for (size_t i = 0; i < items.size(); ++i)
                if (skins::IsChosen(*items[i])) g_group = (skins::Group)k, g_sel[k] = (int)i, g_reveal = true;
        }
        return;
    }
    anims::PreviewHide();
    skins::LetUnusedGo();
    if (!std::exchange(g_captured, false) || core::ConnectMenuVisible()) return;
    In().SetKeyboardCapture(false);
    In().SetMouseCapture(false);
}

bool IsSkinsMenuVisible() { return g_visible; }

void TestSkinsMenu(const std::string& item) {
    if (item == "-") return SetSkinsMenuVisible(false);
    SetSkinsMenuVisible(true);
    for (int k = 0; k < skins::GROUPS; ++k) {
        auto items = Items((skins::Group)k);
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i]->file.empty() ? std::to_string(items[i]->model) == item : items[i]->file == item)
                g_group = (skins::Group)k, g_sel[k] = (int)i, g_scroll = 0, g_view = 0.0f, g_reveal = true;
    }
}

void ToggleSkinsMenu() {
    if (g_visible) return SetSkinsMenuVisible(false);
    if (!core::IsPlayerReady() || game::MenuActive() || core::ConnectMenuVisible() || Gfx().Failed()) return;
    if (std::string why = skins::WhyNot(); !why.empty()) return TheChat().AddError(why);
    SetSkinsMenuVisible(true);
}

void RenderSkinsMenu() {
    if (!g_visible) return;
    if (!core::IsPlayerReady() || game::MenuActive() || core::ConnectMenuVisible() || game::Ref<uint8_t>(0xB5F851))  // a cutscene
        return SetSkinsMenuVisible(false);
    bool first = !g_captured;
    if (first) {
        In().SetKeyboardCapture(true);
        In().SetMouseCapture(true);
        g_captured = true;
    }
    auto& g = Gfx();
    auto& st = In().State();
    float s = g.Scale();
    float lh = g.LineHeight(FONT_NORMAL), sh = g.LineHeight(FONT_SMALL), mh = g.LineHeight(FONT_MENU), ch = g.LineHeight(FONT_MENU_SMALL);
    float pad = 28 * s, room = mh * 1.6f;
    float W = std::min(1180 * s, g.Width() - 32 * s), H = std::min(720 * s, g.Height() - 32 * s);
    float x = (g.Width() - W) * 0.5f, y = std::max(16 * s, (g.Height() - H) * 0.45f);
    float cx = x + pad, cy = y + 22 * s, cw = W - 2 * pad;
    w::BeginFrame();
    g.Rect(x, y, W, H, 0xF0000000);
    g.RectOutline(x, y, W, H, kSaLine, std::max(1.0f, s));
    g.Text(FONT_MENU, cx, cy, TR("Скины", "Skins"), kSaBlue, TEXT_SHADOW);
    cy += mh + 14 * s;

    bool wear = false, moved = false, home = false, end = false;
    int pages = 0;
    if (!first)
        for (int vk : st.keyDowns) {
            if (vk == VK_UP || vk == VK_DOWN) g_sel[g_group] += vk == VK_UP ? -1 : 1, moved = true;
            if (vk == VK_LEFT || vk == VK_RIGHT)
                g_group = (skins::Group)((g_group + (vk == VK_LEFT ? skins::GROUPS - 1 : 1)) % skins::GROUPS), g_scroll = 0, g_view = 0.0f;
            if (vk == VK_RETURN) wear = true;
            if (vk == VK_PRIOR || vk == VK_NEXT) pages += vk == VK_PRIOR ? -1 : 1;
            if (vk == VK_HOME) home = true;
            if (vk == VK_END) end = true;
        }

    float names = 0.0f;
    for (int k = 0; k < skins::GROUPS; ++k) names += g.TextWidth(FONT_MENU_SMALL, skins::GroupName((skins::Group)k));
    float space = std::clamp((cw - names) / (float)skins::GROUPS, 8 * s, 60 * s), tx = cx;
    for (int k = 0; k < skins::GROUPS; ++k) {
        const char* label = skins::GroupName((skins::Group)k);
        float tw = g.TextWidth(FONT_MENU_SMALL, label) + space;
        if (w::Tab("skin-tab", tx, cy, tw, ch + 12 * s, label, g_group == k, true)) g_group = (skins::Group)k, g_scroll = 0, g_view = 0.0f;
        tx += tw;
    }
    cy += ch + 26 * s;

    float rowH = lh + 12 * s;
    float footY = y + H - 18 * s - rowH;
    float bodyH = footY - sh - 22 * s - cy;
    float pv = std::min(bodyH, cw * 0.45f);
    float listW = cw - pv - 24 * s;

    // the list; in the packs' tab each pack's rows go under its title. A row is an item's index, or -1 - i for the
    // title over item i
    auto items = Items(g_group);
    std::vector<int> rows;
    for (int i = 0; i < (int)items.size(); ++i) {
        if (!items[i]->pack.empty() && (i == 0 || items[i]->pack != items[i - 1]->pack)) rows.push_back(-1 - i);
        rows.push_back(i);
    }
    int n = (int)items.size(), nr = (int)rows.size(), shown = std::max(1, (int)(bodyH / rowH));
    int& sel = g_sel[g_group];
    if (pages) sel += pages * std::max(1, shown - 1), moved = true;
    if (home) sel = 0, moved = true;
    if (end) sel = n - 1, moved = true;
    sel = n ? std::clamp(sel, 0, n - 1) : 0;
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
    const Skin* chosen = n ? items[sel] : nullptr;
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
        bool own = !items[i]->file.empty();
        if (!own) g.Text(FONT_SMALL, cx + 58 * s, ry + (rowH - sh) * 0.5f, std::to_string(items[i]->model), kSaMuted, TEXT_RIGHT);
        std::string label = items[i]->name + (skins::IsChosen(*items[i]) ? TR(" · надет", " · worn") : "");
        float lx = cx + (own ? 18 : 74) * s;
        g.Text(FONT_NORMAL, lx, ry + (rowH - lh) * 0.5f, Fit(FONT_NORMAL, label, cx + listW - 16 * s - lx), i == sel ? kSaBlue : kText,
               TEXT_SHADOW);
        if (hover && st.lmbPressed) {
            int row = g_group * 1000 + i;
            if (i == sel && g_clickRow == row && NowMs() - g_clickMs < 450) wear = true;
            sel = i, chosen = items[i], g_clickRow = row, g_clickMs = NowMs();
        }
    }
    g.ClipEnd();
    if (!n && g_group == skins::PACKS)
        g.Text(FONT_SMALL, cx, cy + 6 * s, TR("В папке coop\\skinpacks нет паков скинов.", "There are no skin packs in coop\\skinpacks."),
               kSaMuted, TEXT_SHADOW);
    if (!n && g_group == skins::OWN)
        g.Text(FONT_SMALL, cx, cy + 6 * s, TR("Положите файлы .dff и .txd с одним именем в папку coop\\skins.", "Put a .dff and a .txd of one name into coop\\skins."),
               kSaMuted, TEXT_SHADOW);
    if (nr > shown) {  // where the list is scrolled to
        float bh = listH * (float)shown / (float)nr, by = cy + listH * g_view / (float)nr;
        g.Rect(cx + listW - 4 * s, by, 3 * s, bh, kSaMuted);
    }

    // the preview: the chosen model standing, turned by dragging it; an own skin is loaded to be shown
    std::string broken;
    int model = chosen ? skins::ModelOf(*chosen, &broken) : -1;
    float px = cx + listW + 24 * s, py = y + mh + ch + 62 * s;
    g.Rect(px, py, pv, pv, 0xFF0C1117);
    if (void* tex = model > 0 ? anims::PreviewTexture() : nullptr) g.Image(tex, px, py, pv, pv);
    else if (!broken.empty()) g.Text(FONT_SMALL, px + 14 * s, py + 14 * s, broken, kSaRed, TEXT_SHADOW);
    g.RectOutline(px, py, pv, pv, kSaLine, std::max(1.0f, s));
    if (st.lmb && (g_dragX >= 0.0f || (st.lmbPressed && Hovered(px, py, pv, pv)))) {
        if (g_dragX >= 0.0f) anims::PreviewTurn((st.mouseX - g_dragX) * 0.01f);
        g_dragX = st.mouseX;
    } else {
        g_dragX = -1.0f;
    }
    if (model > 0) anims::PreviewModel(model, pv);

    float ny = footY - sh - 12 * s;
    if (NowMs() < g_noteUntil) g.Text(FONT_SMALL, cx, ny, g_note, g_noteError ? kSaRed : kSaBlue, TEXT_SHADOW);
    else if (g_group == skins::OWN)
        g.Text(FONT_SMALL, cx, ny, TR("Свой скин видят игроки с такими же файлами. Он остаётся и в катсценах.",
                                      "Players with the same files see your own skin. It stays in cutscenes too."), kSaMuted, TEXT_SHADOW);
    else g.Text(FONT_SMALL, cx, ny, TR("Скин видят все игроки. Он остаётся и в катсценах.", "Everybody sees the skin. It stays in cutscenes too."),
                kSaMuted, TEXT_SHADOW);
    const char* wearText = TR("Надеть", "Wear");
    const char* cjText = TR("Вернуть CJ", "CJ again");
    const char* close = TR("Закрыть", "Close");
    float closeX = cx + cw - g.TextWidth(FONT_MENU, close);
    float cjX = closeX - room - 40 * s - g.TextWidth(FONT_MENU, cjText);
    float wearX = cjX - room - 40 * s - g.TextWidth(FONT_MENU, wearText);
    if (w::Button("skin-wear", wearX - room, footY, g.TextWidth(FONT_MENU, wearText) + 2 * room, rowH, wearText, model > 0)) wear = true;
    bool cj = w::Button("skin-cj", cjX - room, footY, g.TextWidth(FONT_MENU, cjText) + 2 * room, rowH, cjText, skins::AnyChosen());
    bool closing = w::Button("skin-close", closeX - room, footY, g.TextWidth(FONT_MENU, close) + 2 * room, rowH, close);
    w::EndFrame();
    w::DrawCursor();
    if (wear && model > 0) WearChosen(chosen);
    else if (cj) WearChosen(nullptr);
    else if (closing) SetSkinsMenuVisible(false);
}

}  // namespace coop::ui
