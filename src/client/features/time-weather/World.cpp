// Time of day and weather follow the host: the host broadcasts them every second, the others correct
// their clock when it drifts (between updates every game advances its own clock at the same rate).
#include <algorithm>
#include <cstdlib>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/session/Flow.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

uint8_t& ClockHours() { return Ref<uint8_t>(0xB70153); }    // CClock::ms_nGameClockHours
uint8_t& ClockMinutes() { return Ref<uint8_t>(0xB70152); }  // CClock::ms_nGameClockMinutes
uint8_t& ClockWeekday() { return Ref<uint8_t>(0xB7014E); }  // CClock::CurrentDay
uint32_t& LastClockTick() { return Ref<uint32_t>(0xB70158); }  // CClock::ms_nLastClockTick (CTimer ms)
uint32_t MsPerGameMinute() { return Ref<uint32_t>(0xB7015C); }
int16_t& OldWeather() { return Ref<int16_t>(0xC81320); }
int16_t& NewWeather() { return Ref<int16_t>(0xC8131C); }
int16_t& ForcedWeather() { return Ref<int16_t>(0xC81318); }
float& WeatherInterpolation() { return Ref<float>(0xC8130C); }

constexpr int kNumWeatherTypes = 23;

// CGameLogic::PassTime moves the clock on after a death or an arrest (12 h), a save (6 h), a trip or a train skip. The
// world's clock is the host's: on the partner the host's clock took it back a second later and the light jumped twice
// (a moment of night after a respawn by day). Its days passed and its pickups still count.
// The host's death or arrest made night fall for everybody in the middle of the day: while partners are in the game,
// that 12 h jump is left out on the host too. A save still passes 6 h: that is how players wait for a mission's hour.
using SetClockFn = void(__cdecl*)(uint8_t, uint8_t, uint8_t);
SetClockFn g_origSetClock = nullptr;
bool g_respawning = false;  // PassTime after a death or an arrest

bool PartnersIn() {
    for (auto& [id, peer] : Net().Peers())
        if (id != Net().MyId()) return true;
    return false;
}

void __cdecl HookPassTimeClock(uint8_t hours, uint8_t minutes, uint8_t day) {
    if (Net().Online() && !Net().IsHost() && flow::InHostWorld()) return;
    if (g_respawning && Net().IsHost() && PartnersIn()) {
        LOGI("Respawn: the clock stays at %02d:%02d (partners in the game)", ClockHours(), ClockMinutes());
        return;
    }
    g_origSetClock(hours, minutes, day);
}

using PassTimeFn = void(__cdecl*)(uint32_t);
PassTimeFn g_origPassTime = (PassTimeFn)0x4414C0;  // CClock::PassTime: all three sites call it
void __cdecl HookRespawnPassTime(uint32_t minutes) {
    g_respawning = true;
    g_origPassTime(minutes);
    g_respawning = false;
}

// A club's dance (dance.scm, a streamed script, no mission) keeps a night of its own behind its fades: STORE_CLOCK,
// SET_TIME_OF_DAY 0 0, RESTORE_CLOCK at its end. The partners took the host's clock every second, and their time ran to
// midnight and back in plain sight as the host began and ended a dance (15.11). While a script of no mission has the
// clock stored, the host sends the stored one: the partners' time waits where the host's comes back to. A story
// mission's hour is the world's and goes as before.
void* g_clockHeldBy = nullptr;  // that script, till its RESTORE_CLOCK or its end
using ClockFn = void(__cdecl*)();
ClockFn g_origStoreClock = nullptr, g_origRestoreClock = nullptr;
void __cdecl HookStoreClock() {
    g_origStoreClock();
    void* s = CurrentScript();
    g_clockHeldBy = s && !Field<uint8_t>(s, 0xDC) ? s : nullptr;  // CRunningScript::m_bIsMission
}
void __cdecl HookRestoreClock() {
    g_origRestoreClock();
    g_clockHeldBy = nullptr;
}
bool ScriptRuns(void* script) {
    for (void* s = Ref<void*>(0xA8B42C); s; s = Field<void*>(s, 0x00))  // CTheScripts::pActiveScripts
        if (s == script) return true;
    return false;
}

class World : public Module {
public:
    const char* Name() const override { return "World"; }

    void OnInstallHooks() override {
        mem::RedirectCallFn(0x441534, &HookPassTimeClock, &g_origSetClock);  // CGameLogic::PassTime -> CClock::SetGameClock
        // PassTime(720): CGameLogic::Update after a death and after an arrest, CGameLogic::ResetStuffUponResurrection
        for (uintptr_t at : {0x442FCF, 0x44337A, 0x4429C5}) mem::RedirectCallFn(at, &HookRespawnPassTime, &g_origPassTime);
        mem::RedirectCallFn(0x47F1AC, &HookStoreClock, &g_origStoreClock);      // STORE_CLOCK -> CClock::StoreClock
        mem::RedirectCallFn(0x47F1C7, &HookRestoreClock, &g_origRestoreClock);  // RESTORE_CLOCK -> CClock::RestoreClock
        Net().SetHandler(proto::G_WORLD_STATE, [](uint8_t from, Reader& r) { Handle(from, r); });
    }

    void OnGameShutdown() override { g_clockHeldBy = nullptr; }

    void OnPostProcess() override {
        if (!Net().IsHost() || !core::IsPlayerReady() || NowMs() - m_lastSend < 1000) return;
        m_lastSend = NowMs();
        if (g_clockHeldBy && !ScriptRuns(g_clockHeldBy)) g_clockHeldBy = nullptr;  // over with no RESTORE_CLOCK
        bool held = g_clockHeldBy != nullptr;
        Writer w(proto::G_WORLD_STATE);
        w.U8(held ? Ref<uint8_t>(0xB7014B) : ClockHours());    // CClock::ms_Stored_nGameClockHours
        w.U8(held ? Ref<uint8_t>(0xB7014A) : ClockMinutes());  // CClock::ms_Stored_nGameClockMinutes
        w.U8(ClockWeekday());
        w.U32(held ? 0 : TimeMs() - LastClockTick());  // progress inside the current game minute
        w.I16(OldWeather());
        w.I16(NewWeather());
        w.I16(ForcedWeather());
        w.F32(WeatherInterpolation());
        Net().Send(proto::CH_STATE, w);
    }

    static void Handle(uint8_t from, Reader& r) {
        uint8_t h = r.U8(), m = r.U8(), weekday = r.U8();
        uint32_t into = r.U32();
        int16_t oldW = r.I16(), newW = r.I16(), forced = r.I16();
        r.F32();  // the host's InterpolationValue: ours comes from our clock (see below)
        if (!r.Ok() || Net().IsHost() || from != Net().HostId() || !core::IsPlayerReady()) return;
        uint32_t per = MsPerGameMinute();
        if (h > 23 || m > 59 || per == 0) return;
        into = std::min(into, per - 1);  // the host's clock may be frozen (cutscene)

        int64_t dayLen = 24LL * 60 * per;
        int64_t host = ((int64_t)h * 60 + m) * per + into;
        int64_t mine = ((int64_t)ClockHours() * 60 + ClockMinutes()) * per + (TimeMs() - LastClockTick());
        int64_t diff = ((host - mine) % dayLen + dayLen + dayLen / 2) % dayLen - dayLen / 2;  // shortest way round
        if (std::llabs(diff) > per / 2) {
            if (std::llabs(diff) > per * 2) LOGI("Clock set to the host's %02d:%02d (was %02d:%02d)", h, m, ClockHours(), ClockMinutes());
            ClockHours() = h;
            ClockMinutes() = m;
            if (weekday >= 1 && weekday <= 7) ClockWeekday() = weekday;
            LastClockTick() = TimeMs() - into;
            // CWeather::Update (0x72B8A1) takes InterpolationValue going down for a new hour and moves the weather on
            // to its own next one: a clock set back did that, and so did the host's value written here every second
            // (a bit ahead of ours with the network's delay), so the partner's weather and colours flipped each second
            uint32_t seconds = into * 60 / per;  // as CClock::Update counts them
            WeatherInterpolation() = std::max(0.0f, (seconds / 60.0f + m) / 60.0f - 1e-5f);
        }
        // weather ids index fixed tables: never take an out-of-range one from the network. The pair is the host's
        // hour's: at the turn of the hour one game is in the next hour first and keeps its own pair until the other is
        // the partner's own side activity forcing its weather (the flight school's, the boat school's) keeps it: the
        // host's, written over it every second, undid it
        if (RunningMission() && ForcedWeather() != -1) return;
        if (h == ClockHours() && oldW >= 0 && oldW < kNumWeatherTypes && newW >= 0 && newW < kNumWeatherTypes) {
            OldWeather() = oldW;
            NewWeather() = newW;
        }
        if (forced >= -1 && forced < kNumWeatherTypes) ForcedWeather() = forced;
    }

private:
    uint64_t m_lastSend = 0;
};

World g_world;
struct Reg {
    Reg() { core::RegisterModule(&g_world); }
} g_reg;
}  // namespace

}  // namespace coop
