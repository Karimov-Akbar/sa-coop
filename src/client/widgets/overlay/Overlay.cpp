#include "widgets/overlay/Overlay.h"

#include <algorithm>

#include "common/config/version/Version.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
#include "features/story/contest/Contest.h"
#include "features/story/missions/Missions.h"
#include "features/teleport/Teleport.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/renderer/Renderer.h"
#include "shared/ui/widgets/Widgets.h"

namespace coop::ui {

void RenderOverlay() {
    auto& g = Gfx();
    float s = g.Scale();
    auto& net = Net();
    bool frontend = game::GameState() != game::GAME_STATE_IDLE;

    // status line, bottom-right
    std::string line;
    if (net.Online()) {
        line = Format(TR("SA:COOP · %s · %u мс", "SA:COOP · %s · %u ms"),
                      net.IsHost() ? TR("хост", "host") : TR("напарник", "partner"), net.Rtt());
    } else if (frontend) {
        line = TR("SA:COOP " COOP_VERSION_STR " · F9: совместная игра", "SA:COOP " COOP_VERSION_STR " · F9: co-op");
    }
    if (!line.empty())
        g.Text(FONT_SMALL, g.Width() - 16 * s, g.Height() - g.LineHeight(FONT_SMALL) - 10 * s, line, 0xC0FFFFFF,
               TEXT_RIGHT | TEXT_OUTLINE);

    // A contest of two players on top of the screen: the activity, both players' points or times (the one ahead in
    // green), our time left to start our own game and how, and at its end the result in big letters a while. A single
    // small line of it was easy to miss, and the time to start and how were in the chat only.
    float boardBottom = 0.0f;
    contest::BoardView b;
    if (!frontend && !core::ConnectMenuVisible() && contest::Board(b)) {
        const char* verdict = b.verdict == 1    ? TR("ПОБЕДА", "YOU WIN")
                              : b.verdict == -1 ? TR("ПОРАЖЕНИЕ", "YOU LOSE")
                              : b.verdict == 2  ? TR("НИЧЬЯ", "A DRAW")
                                                : nullptr;
        uint32_t verdictColor = b.verdict == 1 ? w::kSaGreen : b.verdict == -1 ? w::kSaRed : w::kSaBlue;
        // centred on top, up to 40% of the screen wide: its long texts wrap there. Moved right of the chat's column for
        // them, it stood off the middle, and kept clear of that column it looked squeezed (29.10). Drawn after the chat
        // (Core), it hides the ends of the chat's longest lines while it is there.
        float pad = 14 * s, gap = 4 * s, rowH = g.LineHeight(FONT_NORMAL) + gap, inner = std::max(g.Width() * 0.4f - 2 * pad, 200 * s);
        std::vector<std::wstring> title{Utf8ToWide(Format(TR("Соревнование · %s", "Contest · %s"), b.title.c_str()))};
        if (g.TextWidthW(FONT_MENU_SMALL, title[0]) > inner) {  // the activity on its own lines (a triathlon's long name)
            title = g.Wrap(FONT_MENU_SMALL, Utf8ToWide(b.title), inner);
            title.insert(title.begin(), Utf8ToWide(TR("Соревнование", "Contest")));
        }
        std::vector<std::wstring> wait, how;
        if (!b.wait.empty()) wait = g.Wrap(FONT_SMALL, Utf8ToWide(b.wait), inner);
        if (!b.how.empty()) how = g.Wrap(FONT_SMALL, Utf8ToWide(b.how), inner);
        float names = std::max(g.TextWidth(FONT_NORMAL, b.me.name), g.TextWidth(FONT_NORMAL, b.them.name));
        float values = std::max(g.TextWidth(FONT_NORMAL, b.me.value), g.TextWidth(FONT_NORMAL, b.them.value));
        float bw = std::max(300 * s, verdict ? g.TextWidth(FONT_BIG, verdict) : 0.0f);
        for (auto* lines : {&title, &wait, &how})
            for (auto& l : *lines) bw = std::max(bw, g.TextWidthW(lines == &title ? FONT_MENU_SMALL : FONT_SMALL, l));
        bw = std::max(std::min(bw, inner), names + values + 48 * s) + 2 * pad;  // a long nickname widens it: rows do not wrap
        float small = g.LineHeight(FONT_SMALL);
        float bh = 2 * pad + (verdict ? g.LineHeight(FONT_BIG) + gap : 0.0f) + title.size() * g.LineHeight(FONT_MENU_SMALL) + gap + 2 * rowH +
                   (wait.empty() ? 0.0f : wait.size() * small + gap) + how.size() * small;
        float bx = (g.Width() - bw) * 0.5f, by = 10 * s, cx = g.Width() * 0.5f, cy = by + pad;
        g.Rect(bx, by, bw, bh, 0xC8000000);
        g.RectOutline(bx, by, bw, bh, w::kSaLine, std::max(1.0f, s));
        if (verdict) {
            g.Text(FONT_BIG, cx, cy, verdict, verdictColor, TEXT_CENTER | TEXT_SHADOW);
            cy += g.LineHeight(FONT_BIG) + gap;
        }
        for (auto& l : title) {
            g.TextW(FONT_MENU_SMALL, cx, cy, l, w::kSaBlue, TEXT_CENTER);
            cy += g.LineHeight(FONT_MENU_SMALL);
        }
        cy += gap;
        auto row = [&](const contest::BoardRow& r, bool me) {
            uint32_t color = r.leads ? w::kSaGreen : me ? 0xFFFFFFFF : 0xFFD0D0D0;
            g.Text(FONT_NORMAL, bx + pad, cy, r.name, color, TEXT_SHADOW);
            g.Text(FONT_NORMAL, bx + bw - pad, cy, r.value, color, TEXT_RIGHT | TEXT_SHADOW);
            cy += rowH;
        };
        row(b.me, true);
        row(b.them, false);
        for (auto& l : wait) {
            g.TextW(FONT_SMALL, cx, cy, l, w::kSaBlue, TEXT_CENTER | TEXT_SHADOW);
            cy += small;
        }
        if (!wait.empty()) cy += gap;
        for (auto& l : how) {
            g.TextW(FONT_SMALL, cx, cy, l, w::kSaMuted, TEXT_CENTER | TEXT_SHADOW);
            cy += small;
        }
        boardBottom = by + bh;
    }

    // flow status in the centre while something is going on and the menu is closed (under the contest's board)
    auto phase = flow::GetPhase();
    std::string st, hint;
    if (phase == flow::Phase::WaitingWorld || phase == flow::Phase::StartingServer || phase == flow::Phase::Connecting)
        st = flow::StatusText();
    else if (!frontend && (st = CutsceneSkipText(hint)).empty() && (st = contest::Text(hint)).empty() &&
             (st = teleport::Text(hint)).empty() && (st = SideCallText(hint)).empty())
        st = MissionWaitText(hint);
    if (!core::ConnectMenuVisible() && !st.empty()) {
        float w = std::max(g.TextWidth(FONT_MENU, st), hint.empty() ? 0.0f : g.TextWidth(FONT_SMALL, hint)) + 48 * s;
        float h = g.LineHeight(FONT_MENU) + 24 * s + (hint.empty() ? 0.0f : g.LineHeight(FONT_SMALL) + 6 * s);
        float x = (g.Width() - w) * 0.5f, y = std::max(g.Height() * 0.18f, boardBottom + 10 * s);
        g.Rect(x, y, w, h, 0xD0000000);
        g.Text(FONT_MENU, g.Width() * 0.5f, y + 12 * s, st, w::kSaBlue, TEXT_CENTER | TEXT_SHADOW);
        if (!hint.empty())
            g.Text(FONT_SMALL, g.Width() * 0.5f, y + 18 * s + g.LineHeight(FONT_MENU), hint, w::kSaMuted, TEXT_CENTER | TEXT_SHADOW);
    }

    if (Cfg().debugOverlay) {
        auto& nc = net.Net();
        g.Text(FONT_SMALL, 16 * s, g.Height() * 0.5f,
               Format("net in %llu KB / out %llu KB, state %d, phase %d", (unsigned long long)(nc.BytesIn() / 1024),
                      (unsigned long long)(nc.BytesOut() / 1024), (int)net.GetState(), (int)flow::GetPhase()),
               0xFFFFFF80, TEXT_OUTLINE);
        float y = g.Height() * 0.5f + g.LineHeight(FONT_SMALL);
        if (game::CPlayerPed* me = game::FindPlayerPed(0); me && me->InVehicle() && me->Vehicle()) {
            game::CVehicle* v = me->Vehicle();
            game::CPad* pad = game::GetPad(0);
            g.Text(FONT_SMALL, 16 * s, y,
                   Format("my car: model %d status %d engine %d driver %s gas %.2f speed %.1f cross %d getIn %d/%02X getOut %02X pass %d",
                          v->ModelIndex(), v->Status(), v->EngineOn(), v->Driver() == me ? "me" : "other", v->GasPedal(),
                          v->MoveSpeed().Length() * 50.0f, pad->NewState.ButtonCross,
                          game::Field<uint8_t>(v, 0x485), game::Field<uint8_t>(v, 0x486), game::Field<uint8_t>(v, 0x487),
                          v->NumPassengers()),
                   0xFF80FFFF, TEXT_OUTLINE);
            y += g.LineHeight(FONT_SMALL);
            g.Text(FONT_SMALL, 16 * s, y,
                   Format("   flags %08X %08X wheelsOnGround %d pos %.1f %.1f %.1f pad LX %d LY %d RS1 %d Sq %d", v->VehicleFlagsLo(),
                          v->VehicleFlagsHi(), game::Field<uint8_t>(v, 0x961), v->GetPosition().x, v->GetPosition().y,
                          v->GetPosition().z, pad->NewState.LeftStickX, pad->NewState.LeftStickY, pad->NewState.RightShoulder1,
                          pad->NewState.ButtonSquare),
                   0xFF80FFFF, TEXT_OUTLINE);
            y += g.LineHeight(FONT_SMALL);
        }
        for (auto& [id, rp] : Players::Instance().All()) {
            game::CVector p = rp.ped ? rp.ped->GetPosition() : game::CVector{};
            game::CPlayerPed* me = game::FindPlayerPed(0);
            float d = (me && rp.ped) ? (p - me->GetPosition()).Length() : -1.0f;
            int task = rp.ped ? ActivePrimaryTaskType(rp.ped) : -1;
            int seatHere = rp.ped && rp.ped->InVehicle() ? PassengerSeatOf(rp.ped->Vehicle(), rp.ped) : -9;
            g.Text(FONT_SMALL, 16 * s, y,
                   Format("remote %d: ped %s pos %.1f %.1f %.1f net %.1f %.1f %.1f dist %.1f mode %d veh %04X seat %d here %d task %d", id,
                          rp.ped ? "ok" : "none", p.x, p.y, p.z, rp.state.pos.x, rp.state.pos.y, rp.state.pos.z, d,
                          (int)rp.mode, rp.vehNetId, rp.seat, seatHere, task),
                   0xFFFFFF80, TEXT_OUTLINE);
            y += g.LineHeight(FONT_SMALL);
        }
    }
}

}  // namespace coop::ui
