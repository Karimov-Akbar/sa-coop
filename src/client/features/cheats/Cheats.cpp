// Cheats that change the shared world (weather, game speed, traffic, pedestrians, themes, car behaviour) work in both
// games: the other game applies the same cheat (CCheat::ApplyCheat), a switch only when its own state differs. Cheats
// for the player alone (weapons, money, health, wanted level, vehicles, body, skills) stay in the game where they were
// typed.
#include <cstring>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "entities/session/Flow.h"
#include "common/lib/util/Util.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"

namespace coop {

namespace {
using namespace game;

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
int g_matched = 0;  // cheats matched so far (/cheat tells whether its code matched)
void __cdecl HookCheatCounted(uint16_t stat, float value) {
    g_origIncrementStat(stat, value);
    ++g_matched;
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

    void OnInstallHooks() override {
        mem::RedirectCallFn(0x438565, &HookCheatCounted, &g_origIncrementStat);  // in CCheat::AddToCheatString
        Net().SetHandler(proto::G_CHEAT, [](uint8_t from, Reader& r) {
            int id = r.U8();
            bool on = r.U8() != 0;
            if (!r.Ok() || !WorldCheat(id) || !core::IsPlayerReady() || !flow::InSharedWorld()) return;
            if (OneShot(id) || Active(id) != on) Call<0x438370>(id);  // CCheat::ApplyCheat
            ui::TheChat().AddSystem(
                Format(TR("Чит-код от %s действует и у вас.", "The cheat %s entered works for you too."), Net().PeerName(from).c_str()));
            LOGI("Cheat %d from player %d applied", id, from);
        });
    }

    // /cheat CODE: the letter T opens the chat, so a code with a T could not be typed in the game. The command feeds the
    // code to the game the way its own keyboard check does (CCheat::DoCheats: AddToCheatString per pressed key, the
    // uppercase letter), so the match, the effect and the sharing of world cheats are the game's own.
    bool OnCommand(const std::string& cmd, const std::vector<std::string>& args) override {
        if (cmd != "cheat" && cmd != "c") return false;
        std::string code;
        for (const std::string& a : args)
            for (char c : a) {
                if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) code += c;
            }
        if (code.empty()) {
            ui::TheChat().AddError(TR("Введите код латиницей: /cheat <код>, например /cheat HESOYAM.",
                                      "Type the code in Latin letters: /cheat <code>, for example /cheat HESOYAM."));
            return true;
        }
        if (!core::IsPlayerReady()) {
            ui::TheChat().AddError(TR("Сначала дождитесь загрузки игры.", "Wait until the game has loaded first."));
            return true;
        }
        int before = g_matched;
        for (char c : code) Call<0x438480>(c);  // CCheat::AddToCheatString
        bool worked = g_matched != before;
        if (worked) ui::TheChat().AddSystem(Format(TR("Чит-код %s введён.", "The cheat %s was entered."), code.c_str()));
        else ui::TheChat().AddError(Format(TR("Код %s не подошёл.", "The code %s did not match."), code.c_str()));
        LOGI("Cheat typed by command: %s (%s)", code.c_str(), worked ? "matched" : "no match");
        return true;
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
