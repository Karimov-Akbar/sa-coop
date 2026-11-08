// Weapons as gifts (the F2 window): a gun with some or all of its rounds goes to another player, as money does
// (Money.cpp). It leaves the giver at once and is given in the other game, which says it came. A slot holds one weapon:
// a gift for a slot that holds another one there comes back, so that player's own gun is not thrown away. So does one
// that never came (the other player left first). A gift that comes while the world loads waits for the world.
#include "features/weapon-gift/WeaponGift.h"

#include <algorithm>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/profile/Profile.h"
#include "entities/session/Flow.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"

namespace coop::weapon_gift {

namespace {
using namespace game;
using game::script::Cmd;

enum : uint8_t { OP_GIVE = 1, OP_GOT = 2, OP_BACK = 3 };
constexpr int kMaxAmmo = 99999;  // CWeapon's own limit

struct Gift {
    uint8_t peer;
    int type, ammo;
    bool back;  // ours coming back, not a gift
};
std::vector<Gift> g_unsure;    // ours not come there yet
std::vector<Gift> g_incoming;  // come while our world was loading

bool Ready() { return core::IsPlayerReady() && flow::InSharedWorld() && !flow::HostWorldLoading(); }

// eWeaponType has no 19-21 (rockets, the bomb), and the detonator goes with the satchels already thrown
bool Giftable(int type) { return type >= 1 && type <= 46 && (type < 19 || type > 21) && type != 40; }
void* Info(int type) { return Call<0x743C60, void*>(type, (uint8_t)1); }  // CWeaponInfo::GetWeaponInfo, standard skill
int SlotOf(int type) {
    void* info = Info(type);
    int slot = info ? Field<int32_t>(info, 0x14) : -1;  // m_nSlot
    return slot >= 0 && slot < NUM_WEAPON_SLOTS ? slot : -1;
}
bool Whole(int type, int ammo) {
    void* info = Info(type);
    return !info || Field<int32_t>(info, 0) == 0 || ammo <= 1;  // m_nWeaponFire: FIRE_MELEE
}

void Send(uint8_t op, uint8_t to, int type, int ammo) {
    Writer w(proto::G_WEAPON_GIFT);
    w.U8(op);
    w.U8(to);
    w.U8((uint8_t)type);
    w.I32(ammo);
    Net().Send(proto::CH_RELIABLE, w);
}

// the game's way: a weapon of the same kind gets the rounds added
void GiveUs(int type, int ammo) {
    int model = -1;
    Cmd(0x0781).I(type).Out(&model).Run();  // GET_WEAPONTYPE_MODEL
    if (model > 0) LoadModelNow(model);
    Cmd(0x01B2).I(GetPedRef(FindPlayerPed(0))).I(type).I(std::clamp(ammo, 1, kMaxAmmo)).Run();  // GIVE_WEAPON_TO_CHAR
    if (!Net().IsHost()) SaveProfileNow();  // a world reload puts the profile's weapons back
}

std::string WithAmmo(int type, int ammo, bool whole) {
    return whole ? std::string(Name(type)) : Format(TR("%s (патроны: %d)", "%s (ammo: %d)"), Name(type), ammo);
}

void Take(const Gift& g) {
    std::string from = Net().PeerName(g.peer);
    if (g.back) {
        GiveUs(g.type, g.ammo);
        ui::TheChat().AddSystem(Format(TR("%s вернулось к вам.", "%s came back to you."), Name(g.type)));
        LOGI("Weapon %d x%d given to player %d came back", g.type, g.ammo, g.peer);
        return;
    }
    int slot = SlotOf(g.type);
    const CWeapon* had = slot >= 0 ? &FindPlayerPed(0)->Weapons()[slot] : nullptr;
    if (!had || (had->type != 0 && had->type != g.type)) {
        Send(OP_BACK, g.peer, g.type, g.ammo);
        if (had)
            ui::TheChat().AddSystem(Format(TR("%s хочет передать вам %s, но на этом месте у вас %s. Оружие вернулось к игроку.",
                                              "%s wants to give you %s, but you carry %s in its place. The weapon went back."),
                                           from.c_str(), Name(g.type), Name(had->type)));
        LOGI("Weapon %d x%d from player %d: slot %d holds weapon %d, sent back", g.type, g.ammo, g.peer, slot,
             had ? had->type : -1);
        return;
    }
    GiveUs(g.type, g.ammo);
    Send(OP_GOT, g.peer, g.type, g.ammo);
    ui::TheChat().AddSystem(Format(TR("%s передаёт вам %s.", "%s gives you %s."), from.c_str(),
                                   WithAmmo(g.type, g.ammo, Whole(g.type, g.ammo)).c_str()));
    LOGI("Weapon %d x%d from player %d", g.type, g.ammo, g.peer);
}

void Handle(uint8_t from, Reader& r) {
    uint8_t op = r.U8(), to = r.U8();
    int type = r.U8();
    int32_t ammo = r.I32();
    if (!r.Ok() || from == Net().MyId() || to != Net().MyId() || !Giftable(type) || ammo <= 0 || ammo > kMaxAmmo) return;
    if (op == OP_GIVE) {
        Gift g{from, type, ammo, false};
        if (Ready()) Take(g);
        else g_incoming.push_back(g);
        return;
    }
    auto it = std::find_if(g_unsure.begin(), g_unsure.end(),
                           [&](const Gift& g) { return g.peer == from && g.type == type && g.ammo == ammo; });
    if (it == g_unsure.end() || (op != OP_GOT && op != OP_BACK)) return;
    g_unsure.erase(it);
    if (op == OP_BACK) g_incoming.push_back({from, type, ammo, true});
}

class WeaponGift : public Module {
public:
    const char* Name() const override { return "WeaponGift"; }
    void OnInstallHooks() override { Net().SetHandler(proto::G_WEAPON_GIFT, &Handle); }
    void OnSessionEnd() override { g_unsure.clear(), g_incoming.clear(); }
    void OnPeerLeave(uint8_t id) override {
        for (auto it = g_unsure.begin(); it != g_unsure.end();) {
            if (it->peer != id) {
                ++it;
                continue;
            }
            g_incoming.push_back({id, it->type, it->ammo, true});
            LOGI("Weapon %d x%d to player %d never came: given back", it->type, it->ammo, id);
            it = g_unsure.erase(it);
        }
    }
    void OnPostProcess() override {
        if (g_incoming.empty() || !Net().Online() || !Ready()) return;
        std::vector<Gift> now;
        now.swap(g_incoming);
        for (const Gift& g : now) Take(g);
    }

    static WeaponGift& Instance() {
        static WeaponGift m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&WeaponGift::Instance()); }
} g_reg;
}  // namespace

std::vector<Owned> Ours() {
    std::vector<Owned> out;
    CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
    for (int i = 0; me && i < NUM_WEAPON_SLOTS; ++i) {
        const CWeapon& w = me->Weapons()[i];
        bool whole = Giftable(w.type) && Whole(w.type, w.ammoTotal);
        if (Giftable(w.type) && (whole || w.ammoTotal > 0)) out.push_back({w.type, w.ammoTotal, whole});
    }
    return out;
}

const char* Name(int type) {
    switch (type) {
        case 1: return TR("Кастет", "Brass knuckles");
        case 2: return TR("Клюшка для гольфа", "Golf club");
        case 3: return TR("Дубинка", "Nightstick");
        case 4: return TR("Нож", "Knife");
        case 5: return TR("Бита", "Baseball bat");
        case 6: return TR("Лопата", "Shovel");
        case 7: return TR("Кий", "Pool cue");
        case 8: return TR("Катана", "Katana");
        case 9: return TR("Бензопила", "Chainsaw");
        case 10: return TR("Фиолетовый дилдо", "Purple dildo");
        case 11: return TR("Дилдо", "Dildo");
        case 12: return TR("Вибратор", "Vibrator");
        case 13: return TR("Серебряный вибратор", "Silver vibrator");
        case 14: return TR("Цветы", "Flowers");
        case 15: return TR("Трость", "Cane");
        case 16: return TR("Гранаты", "Grenades");
        case 17: return TR("Слезоточивый газ", "Tear gas");
        case 18: return TR("Коктейли Молотова", "Molotov cocktails");
        case 22: return TR("Пистолет", "Pistol");
        case 23: return TR("Пистолет с глушителем", "Silenced pistol");
        case 24: return TR("Desert Eagle", "Desert Eagle");
        case 25: return TR("Дробовик", "Shotgun");
        case 26: return TR("Обрез", "Sawn-off shotgun");
        case 27: return TR("Боевой дробовик", "Combat shotgun");
        case 28: return TR("Узи", "Micro SMG");
        case 29: return TR("MP5", "MP5");
        case 30: return TR("АК-47", "AK-47");
        case 31: return TR("M4", "M4");
        case 32: return TR("Tec-9", "Tec-9");
        case 33: return TR("Охотничья винтовка", "Country rifle");
        case 34: return TR("Снайперская винтовка", "Sniper rifle");
        case 35: return TR("Гранатомёт", "Rocket launcher");
        case 36: return TR("Самонаводящийся гранатомёт", "Heat-seeking rocket launcher");
        case 37: return TR("Огнемёт", "Flamethrower");
        case 38: return TR("Миниган", "Minigun");
        case 39: return TR("Взрывчатка", "Satchel charges");
        case 41: return TR("Баллончик с краской", "Spray can");
        case 42: return TR("Огнетушитель", "Fire extinguisher");
        case 43: return TR("Фотоаппарат", "Camera");
        case 44: return TR("Очки ночного видения", "Night-vision goggles");
        case 45: return TR("Тепловизор", "Thermal goggles");
        case 46: return TR("Парашют", "Parachute");
        default: return TR("Оружие", "Weapon");
    }
}

bool Give(uint8_t to, int type, int ammo, std::string& error) {
    CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
    int slot = Giftable(type) ? SlotOf(type) : -1;
    CWeapon* w = me && slot >= 0 ? &me->Weapons()[slot] : nullptr;
    if (!Net().Online() || !Net().Peers().count(to)) {
        error = TR("Этого игрока уже нет в игре.", "That player has left.");
    } else if (!me || !flow::InSharedWorld()) {
        error = TR("Передать оружие можно, когда вы в игре.", "You can give a weapon once you are in the game.");
    } else if (!w || w->type != type) {
        error = TR("Этого оружия у вас уже нет.", "You no longer have that weapon.");
    } else {
        bool whole = Whole(type, w->ammoTotal);
        if (whole) ammo = std::max(w->ammoTotal, 1);
        if (ammo <= 0) {
            error = TR("Введите число патронов.", "Enter the number of rounds.");
        } else if (!whole && ammo > w->ammoTotal) {
            error = Format(TR("Столько патронов у вас нет: есть %d.", "You do not have that many rounds: %d."), w->ammoTotal);
        } else {
            if (whole || ammo >= w->ammoTotal) {
                Cmd(0x0555).I(GetPedRef(me)).I(type).Run();  // REMOVE_WEAPON_FROM_CHAR
            } else {
                w->ammoTotal -= ammo;
                w->ammoInClip = std::min(w->ammoInClip, w->ammoTotal);
            }
            g_unsure.push_back({to, type, ammo, false});
            Send(OP_GIVE, to, type, ammo);
            ui::TheChat().AddSystem(Format(TR("Вы передали %s игроку %s.", "You gave %s to %s."), WithAmmo(type, ammo, whole).c_str(),
                                           Net().PeerName(to).c_str()));
            LOGI("Weapon %d x%d to player %d", type, ammo, to);
            if (!Net().IsHost()) SaveProfileNow();
            return true;
        }
    }
    return false;
}

}  // namespace coop::weapon_gift
