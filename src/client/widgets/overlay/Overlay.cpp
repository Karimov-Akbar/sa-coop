#include "widgets/overlay/Overlay.h"

#include <algorithm>

#include "common/config/version/Version.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
#include "features/race/Race.h"
#include "features/story/missions/Missions.h"
#include "features/voice/Voice.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/renderer/Renderer.h"
#include "shared/ui/widgets/Widgets.h"

namespace coop::ui {

namespace {
bool g_playerList = false;
}

void TogglePlayerList() { g_playerList = !g_playerList; }

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

    // flow status in the centre while something is going on and the menu is closed
    auto phase = flow::GetPhase();
    std::string st, hint;
    if (phase == flow::Phase::WaitingWorld || phase == flow::Phase::StartingServer || phase == flow::Phase::Connecting)
        st = flow::StatusText();
    else if (!frontend && (st = CutsceneSkipText(hint)).empty() && (st = MissionWaitText()).empty())
        st = RaceStatusText();
    if (!core::ConnectMenuVisible() && !st.empty()) {
        float w = std::max(g.TextWidth(FONT_MENU, st), hint.empty() ? 0.0f : g.TextWidth(FONT_SMALL, hint)) + 48 * s;
        float h = g.LineHeight(FONT_MENU) + 24 * s + (hint.empty() ? 0.0f : g.LineHeight(FONT_SMALL) + 6 * s);
        float x = (g.Width() - w) * 0.5f, y = g.Height() * 0.18f;
        g.Rect(x, y, w, h, 0xD0000000);
        g.Text(FONT_MENU, g.Width() * 0.5f, y + 12 * s, st, w::kSaBlue, TEXT_CENTER | TEXT_SHADOW);
        if (!hint.empty())
            g.Text(FONT_SMALL, g.Width() * 0.5f, y + 18 * s + g.LineHeight(FONT_MENU), hint, w::kSaMuted, TEXT_CENTER | TEXT_SHADOW);
    }

    if (g_playerList && net.Online()) {
        float W = 520 * s;
        float lh = g.LineHeight(FONT_NORMAL) + 6 * s;
        int rows = 1 + (int)net.Peers().size();
        float H = lh * (rows + 1) + 30 * s;
        float x = (g.Width() - W) * 0.5f, y = g.Height() * 0.25f;
        g.Rect(x, y, W, H, 0xE0000000);
        g.RectOutline(x, y, W, H, w::kSaLine, std::max(1.0f, s));
        g.Text(FONT_MENU_SMALL, x + 16 * s, y + 12 * s,
               Format(TR("%s · игроков: %d", "%s · players: %d"), net.ServerName().c_str(), rows), w::kSaMuted);
        float cy = y + 10 * s + lh * 1.3f;
        auto row = [&](const std::string& name, bool host, uint32_t ping, bool me, bool talking) {
            std::string shown = (talking ? "{FFDC50}((•)){FFFFFF} " : "") + name + (me ? TR(" (вы)", " (you)") : "");
            g.Text(FONT_NORMAL, x + 16 * s, cy, shown, me ? 0xFFFFFFFF : 0xFFD0D0D0, TEXT_SHADOW);
            // the role ends before the ping column ("999 мс"): "напарник" in this font reached into it
            g.Text(FONT_MENU_SMALL, x + W - 104 * s, cy + 3 * s, host ? TR("хост", "host") : TR("напарник", "partner"),
                   host ? w::kSaBlue : w::kSaGreen, TEXT_RIGHT);
            g.Text(FONT_NORMAL, x + W - 16 * s, cy, Format(TR("%u мс", "%u ms"), ping), w::kSaMuted, TEXT_RIGHT | TEXT_SHADOW);
            cy += lh;
        };
        row(Cfg().nickname, net.IsHost(), net.Rtt(), true, VoiceIsTransmitting());
        for (auto& [id, p] : net.Peers()) row(p.name, p.isHost, p.ping, false, VoiceIsSpeaking(id));
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
