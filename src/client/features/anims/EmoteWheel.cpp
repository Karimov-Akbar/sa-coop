// The emote wheel: while its key is held ([keys] emote_wheel, J), the emotes stand in a ring around the middle of the
// screen and the mouse picks one by its direction, as the weapon wheel of the later games; letting the key go plays it.
// The keyboard stays the game's (the player can walk on). The mouse does not: the game's own reading of it goes on (an
// OS cursor does not move in the game, DirectInput owns the mouse there), and its motion moves a cursor of the wheel,
// held inside the ring, instead of the camera (Input::SetMouseRelative). Its direction from the middle picks.
#include <windows.h>

#include <algorithm>
#include <cmath>
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

namespace coop {

namespace {
constexpr float kPi = 3.14159265f;

class EmoteWheel : public Module {
public:
    const char* Name() const override { return "EmoteWheel"; }

    void OnPostProcess() override {
        bool held = game::IsForeground() && (GetAsyncKeyState(Cfg().emoteKey) & 0x8000);
        bool pressed = held && !m_held;
        m_held = held;
        bool usable = core::IsPlayerReady() && !game::MenuActive() && !ui::TheChat().IsOpen() && !core::ConnectMenuVisible();
        if (!m_open) {
            if (!pressed || !usable) return;
            if (const char* why = emotes::Blocked()) {
                if (*why) ui::TheChat().AddError(why);
                return;
            }
            m_items.clear();
            for (int i = 0; i < emotes::Count(); ++i)
                if (emotes::Get(i).wheel) m_items.push_back(i);
            if (m_items.empty()) return;
            m_open = true;
            m_pick = -1;
            m_curX = m_curY = 0;
            ui::In().SetMouseRelative(true);
            return;
        }
        if (!usable) return Close(false);
        MoveCursor();
        m_pick = Hovered();
        if (!held) Close(true);
    }

    void OnRender() override {
        if (!m_open) return;
        auto& g = ui::Gfx();
        float s = g.Scale(), cx = g.Width() * 0.5f, cy = g.Height() * 0.5f;
        float lhName = g.LineHeight(ui::FONT_NORMAL), lhCmd = g.LineHeight(ui::FONT_SMALL);
        float bw = 240 * s, bh = lhName + lhCmd + 10 * s;
        int n = (int)m_items.size();
        // the tiles at the top and the bottom stand closest: far enough for them not to touch, inside the screen
        float r = std::max(330 * s, bh / (1.0f - std::cos(2 * kPi / n)) + 4 * s);
        r = std::min({r, cy - bh / 2 - 8 * s, cx - bw / 2 - 8 * s});
        g.Rect(0, 0, g.Width(), g.Height(), 0x50000000);
        for (int k = 0; k < n; ++k) {
            float a = 2 * kPi * k / n, x = cx + r * std::sin(a), y = cy - r * std::cos(a);
            bool lit = k == m_pick;
            g.Rect(x - bw / 2, y - bh / 2, bw, bh, lit ? 0xE8000000 : 0xC0000000);
            g.RectOutline(x - bw / 2, y - bh / 2, bw, bh, lit ? ui::w::kSaBlue : ui::w::kSaLine, std::max(1.0f, (lit ? 3 : 1) * s));
            std::string label = emotes::Name(m_items[k]);
            if (m_items[k] == emotes::Current()) label = "{5FA04E}" + label;
            float top = y - bh / 2 + 4 * s;
            g.Text(ui::FONT_NORMAL, x, top, label, lit ? ui::w::kSaBlue : 0xFFE0E0E0, ui::TEXT_CENTER | ui::TEXT_SHADOW);
            // its chat commands, both when they fit
            std::string cmd = emotes::Commands(m_items[k]);
            if (g.TextWidth(ui::FONT_SMALL, cmd) > bw - 8 * s) cmd = emotes::Commands(m_items[k], true);
            g.Text(ui::FONT_SMALL, x, top + lhName, cmd, ui::w::kSaMuted, ui::TEXT_CENTER | ui::TEXT_SHADOW);
        }
        // the middle: what the mouse points at and a dot on the way to it
        float lh = g.LineHeight(ui::FONT_MENU_SMALL);
        g.Text(ui::FONT_MENU_SMALL, cx, cy - lh * 1.6f, TR("Эмоции", "Emotes"), ui::w::kSaMuted, ui::TEXT_CENTER | ui::TEXT_SHADOW);
        std::string what = m_pick >= 0 ? emotes::Name(m_items[m_pick]) : TR("Наведите мышь", "Point the mouse");
        g.Text(ui::FONT_NORMAL, cx, cy - g.LineHeight(ui::FONT_NORMAL) * 0.5f, what, m_pick >= 0 ? ui::w::kSaBlue : 0xFFE0E0E0,
               ui::TEXT_CENTER | ui::TEXT_SHADOW);
        if (m_pick >= 0) {
            std::string key = emotes::KeyName(Cfg().emoteKey);
            g.Text(ui::FONT_SMALL, cx, cy + lh * 0.9f, Format(TR("Отпустите %s, чтобы начать", "Release %s to play"), key.c_str()),
                   ui::w::kSaMuted, ui::TEXT_CENTER | ui::TEXT_SHADOW);
            float a = 2 * kPi * m_pick / n, d = r * 0.62f, dot = 10 * s;
            g.Rect(cx + d * std::sin(a) - dot / 2, cy - d * std::cos(a) - dot / 2, dot, dot, ui::w::kSaBlue);
        }
        m_radius = r;
        ui::w::DrawCursorAt(cx + m_curX, cy + m_curY);  // the F9 window's
    }

    void OnGameShutdown() override { Close(false); }

private:
    // the mouse's motion since the last frame, as the camera's sensitivity of the game's settings would turn it
    void MoveCursor() {
        float dx, dy;
        ui::In().TakeMouseMotion(dx, dy);
        float accel = game::Ref<float>(0xB6EC1C);  // CCamera::m_fMouseAccelHorzntl, 0.0025 by default
        float k = std::isfinite(accel) && accel > 0 ? std::clamp(accel / 0.0025f, 0.4f, 3.0f) : 1.0f;
        k *= std::max(0.5f, ui::Gfx().Scale());
        m_curX += dx * k;
        m_curY += dy * k;
        float len = std::sqrt(m_curX * m_curX + m_curY * m_curY), lim = m_radius > 0 ? m_radius : 200 * ui::Gfx().Scale();
        if (len > lim) {  // held inside the ring, as the weapon wheel of the later games
            m_curX *= lim / len;
            m_curY *= lim / len;
        }
    }

    // the item the wheel's cursor points at from the middle of the screen, -1 near the middle
    int Hovered() const {
        auto& g = ui::Gfx();
        float dx = m_curX, dy = m_curY;
        if (dx * dx + dy * dy < (60 * g.Scale()) * (60 * g.Scale())) return -1;
        int n = (int)m_items.size();
        float a = std::atan2(dx, -dy);  // 0 at the top, clockwise
        if (a < 0) a += 2 * kPi;
        return (int)std::lround(a / (2 * kPi / n)) % n;
    }

    void Close(bool play) {
        if (!m_open) return;
        m_open = false;
        ui::In().SetMouseRelative(false);
        if (play && m_pick >= 0) emotes::Play(m_items[m_pick]);
        m_pick = -1;
    }

    bool m_held = false, m_open = false;
    int m_pick = -1;
    float m_curX = 0, m_curY = 0;  // the wheel's cursor from the middle of the screen, in screen pixels
    float m_radius = 0;            // of the ring as last drawn
    std::vector<int> m_items;  // emote indexes, clockwise from the top
};

EmoteWheel g_wheel;
struct Reg {
    Reg() { core::RegisterModule(&g_wheel); }
} g_reg;
}  // namespace

}  // namespace coop
