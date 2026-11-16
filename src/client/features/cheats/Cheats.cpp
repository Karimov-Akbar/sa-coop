// Cheats that change the shared world (weather, game speed, traffic, pedestrians, themes, car behaviour) work in both
// games: the other game applies the same cheat (CCheat::ApplyCheat), a switch only when its own state differs. Cheats
// for the player alone (weapons, money, health, wanted level, vehicles, body, skills) stay in the game where they were
// typed, and so do a theme's clothes and katana. The host may turn the partners' cheat codes off.
#include <cstring>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "entities/session/Flow.h"
#include "common/lib/util/Util.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

constexpr uintptr_t kCheatString = 0x969110;   // CCheat::m_CheatString[30], the newest key first
constexpr uintptr_t kCheatsActive = 0x969130;  // CCheat::m_aCheatsActive[92]
constexpr uintptr_t kHashKeys = 0x8A5CC8;      // CCheat::m_aCheatHashKeys[92]
constexpr int kCheats = 92;

bool WorldCheat(int id) {
    switch (id) {
    case 6: case 7: case 8: case 9: case 10:  // weather
    case 11: case 12: case 13:                // faster clock, faster and slower gameplay
    case 14: case 15: case 16:                // mayhem, bounty, everyone armed
    case 26: case 27: case 28:                // blow up all cars, invisible cars, perfect handling
    case 30: case 31: case 32: case 33: case 34: case 35:  // green lights, aggressive drivers, pink/black traffic, water
    case 39: case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47: case 48: case 49:  // themes, gangs
    case 53: case 54: case 55: case 56: case 57: case 58:  // nitro, floating cars, midnight, orange sky, storms
    case 69: case 70: case 74: case 75:       // riot, funhouse, reduced and country traffic
        return true;
    }
    return false;
}
bool OneShot(int id) { return (id >= 6 && id <= 10) || id == 12 || id == 13 || id == 26 || id == 57 || id == 58; }
bool Active(int id) { return Ref<uint8_t>(kCheatsActive + id) != 0; }

// the cheat the game just matched: its own search over the typed string (CCheat::AddToCheatString), the newest keys
// first and the longest run first
int Matched(const char* s) {
    for (int len = (int)strnlen(s, 29); len >= 6; --len) {
        uint32_t hash = Call<0x53CED0, uint32_t>(s, len);  // CKeyGen::GetKey(const char*, int)
        for (int j = 0; j < kCheats; ++j)
            if (Ref<uint32_t>(kHashKeys + (uintptr_t)j * 4) == hash) return j;
    }
    return -1;
}

// CCheat::AddToCheatString counts a matched cheat (CStats::IncrementStat(STAT_TIMES_CHEATED)) right before applying
// it, with the typed string still whole. The function's own first bytes stay as they are: GInput reads the
// cutscene flag's address out of them and crashed on a hook there.
using IncrementStatFn = void(__cdecl*)(uint16_t, float);
IncrementStatFn g_origIncrementStat = nullptr;
void __cdecl HookCheatCounted(uint16_t stat, float value) {
    g_origIncrementStat(stat, value);
    if (!flow::InSharedWorld()) return;
    int id = Matched((const char*)kCheatString);
    if (id < 0 || !WorldCheat(id)) return;
    Writer w(proto::G_CHEAT);
    w.U8((uint8_t)id);
    w.U8(!Active(id));  // applied right after this: a switch flips
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("Cheat %d typed: sent", id);
}

class Cheats : public Module {
public:
    const char* Name() const override { return "Cheats"; }

    // The host turned its partners' cheat codes off (#118): the keys typed here are forgotten every frame before
    // CPad::DoCheats (CGame::Process) adds the new ones, and CCheat::AddToCheatString looks for a code only in 6 keys or
    // more. The world cheats the host types still come here (G_CHEAT).
    void OnPreProcess() override {
        if (Net().Online() && !Net().IsHost() && !Net().PartnerCheats()) memset((void*)kCheatString, 0, 30);
    }

    void OnInstallHooks() override {
        mem::RedirectCallFn(0x438565, &HookCheatCounted, &g_origIncrementStat);  // in CCheat::AddToCheatString
        Net().SetHandler(proto::G_CHEAT, [](uint8_t from, Reader& r) {
            int id = r.U8();
            bool on = r.U8() != 0;
            if (!r.Ok() || !WorldCheat(id) || !core::IsPlayerReady() || !flow::InSharedWorld()) return;
            CPlayerPed* me = FindPlayerPed(0);
            CPedClothesDesc look = *me->ClothesDesc();
            int melee = me->Weapons()[1].type, current = me->ActiveWeapon().type;  // the hand weapon's slot
            if (OneShot(id) || Active(id) != on) Call<0x438370>(id);  // CCheat::ApplyCheat
            // A theme dresses whoever applies it (the beach party, the gimp suit, the funhouse, country) and the ninja one
            // arms him with a katana: the theme is the world's, the look and the katana the typing player's (issue #43)
            if (memcmp(me->ClothesDesc(), &look, sizeof look) != 0) {
                *me->ClothesDesc() = look;
                if (me->PedState() != 50) Call<0x5A82C0>(me, false);  // not PEDSTATE_DRIVING: CClothes::RebuildPlayer
                LOGD("Cheat %d: our clothes kept", id);
            }
            if (int h = GetPedRef(me); me->Weapons()[1].type == 8 && melee != 8) {  // WEAPON_KATANA
                Cmd(0x0555).I(h).I(8).Run();                         // REMOVE_WEAPON_FROM_CHAR
                if (melee) Cmd(0x01B2).I(h).I(melee).I(1).Run();     // GIVE_WEAPON_TO_CHAR: its own back
                Cmd(0x01B9).I(h).I(current).Run();                   // SET_CURRENT_CHAR_WEAPON
                LOGD("Cheat %d: its katana taken back (%d in hand)", id, current);
            }
            ui::TheChat().AddSystem(
                Format(TR("Чит-код от %s действует и у вас.", "The cheat %s entered works for you too."), Net().PeerName(from).c_str()));
            LOGI("Cheat %d from player %d applied", id, from);
        });
    }

    static Cheats& Instance() {
        static Cheats m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Cheats::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
