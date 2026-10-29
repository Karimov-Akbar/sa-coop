// The players' money, for the F2 window, and gifts of it. Each game says what its player has whenever that changes (twice
// a second at most, and every 10 s anyway). A gift leaves the giver's money at once and is added in the other game, which
// says it came; one that never came (the other player left first) goes back to the giver. A gift that comes while the
// world loads waits for the world. The host also shares with its partners what Big Smoke's courier paid it (Courier.cpp).
#include "features/money/Money.h"

#include <algorithm>
#include <map>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/profile/Profile.h"
#include "entities/session/Flow.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"

namespace coop::money {

namespace {
using namespace game;

enum : uint8_t { OP_NOW = 1, OP_GIVE = 2, OP_GOT = 3, OP_SHARE = 4 };
constexpr int32_t kMaxMoney = 999999999;  // the HUD's nine digits

std::map<uint8_t, int32_t> g_money;    // the other players'
std::map<uint8_t, int64_t> g_unsure;   // our gifts to each that have not come there yet
struct Incoming {
    uint8_t from;
    int32_t amount;
    bool share;  // the courier's money, no gift
};
std::vector<Incoming> g_incoming;  // come while our world was loading
int32_t g_sent = INT32_MIN;
uint64_t g_sentMs = 0;

int32_t& Mine() { return PlayerInfos()[0].money; }
bool Ready() { return core::IsPlayerReady() && flow::InSharedWorld() && !flow::HostWorldLoading(); }

void Send(uint8_t op, uint8_t to, int32_t amount) {
    Writer w(proto::G_MONEY);
    w.U8(op);
    if (op == OP_GIVE || op == OP_GOT) w.U8(to);
    w.I32(amount);
    Net().Send(proto::CH_RELIABLE, w);
}

void Take(const Incoming& in) {
    Mine() = (int32_t)std::min<int64_t>((int64_t)Mine() + in.amount, kMaxMoney);
    if (in.share) {
        ui::TheChat().AddSystem(Format(TR("Ваша доля денег курьера: $%d.", "Your share of the courier's cash: $%d."), in.amount));
        LOGI("Courier share: $%d", in.amount);
    } else {
        Send(OP_GOT, in.from, in.amount);
        ui::TheChat().AddSystem(Format(TR("%s передаёт вам $%d.", "%s gives you $%d."), Net().PeerName(in.from).c_str(), in.amount));
        LOGI("Money from player %d: $%d", in.from, in.amount);
    }
    if (!Net().IsHost()) SaveProfileNow();  // a world reload puts the profile's money back
}

void Handle(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    uint8_t to = op == OP_NOW ? 0xFF : r.U8();
    int32_t amount = r.I32();
    if (!r.Ok() || from == Net().MyId()) return;
    if (op == OP_NOW) {
        g_money[from] = amount;
    } else if ((op == OP_GIVE || op == OP_SHARE) && amount > 0 && amount <= kMaxMoney) {
        if (to != Net().MyId() || (op == OP_SHARE && from != Net().HostId())) return;
        Incoming in{from, amount, op == OP_SHARE};
        if (Ready()) Take(in);
        else g_incoming.push_back(in);
    } else if (op == OP_GOT && to == Net().MyId()) {
        if (auto it = g_unsure.find(from); it != g_unsure.end() && (it->second -= amount) <= 0) g_unsure.erase(it);
    }
}

class Money : public Module {
public:
    const char* Name() const override { return "Money"; }
    void OnInstallHooks() override { Net().SetHandler(proto::G_MONEY, &Handle); }
    void OnSessionEnd() override { g_money.clear(), g_unsure.clear(), g_incoming.clear(), g_sent = INT32_MIN; }
    void OnPeerJoin(uint8_t) override { g_sent = INT32_MIN; }  // the newcomer hears ours at once
    void OnPeerLeave(uint8_t id) override {
        g_money.erase(id);
        auto it = g_unsure.find(id);
        if (it == g_unsure.end()) return;
        int32_t back = (int32_t)it->second;
        g_unsure.erase(it);
        if (back <= 0 || !core::IsPlayerReady()) return;
        Mine() = (int32_t)std::min<int64_t>((int64_t)Mine() + back, kMaxMoney);
        ui::TheChat().AddSystem(Format(TR("Деньги не дошли до игрока %s: $%d вернулись к вам.", "The money did not reach %s: $%d came back to you."),
                                       Net().PeerName(id).c_str(), back));
        LOGI("Gift of $%d to player %d never came: given back", back, id);
    }
    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady()) return;
        if (!g_incoming.empty() && Ready()) {
            for (const Incoming& in : g_incoming) Take(in);
            g_incoming.clear();
        }
        uint64_t now = NowMs();
        if (Net().Peers().empty() || (Mine() == g_sent && now - g_sentMs < 10000) || now - g_sentMs < 500) return;
        g_sent = Mine(), g_sentMs = now;
        Send(OP_NOW, 0xFF, g_sent);
    }

    static Money& Instance() {
        static Money m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Money::Instance()); }
} g_reg;
}  // namespace

bool Of(uint8_t id, int32_t& money) {
    auto it = g_money.find(id);
    if (it == g_money.end()) return false;
    money = it->second;
    return true;
}

bool Give(uint8_t to, int32_t amount, std::string& error) {
    int32_t theirs = 0;
    if (!Net().Online() || !Net().Peers().count(to)) {
        error = TR("Этого игрока уже нет в игре.", "That player has left.");
    } else if (!core::IsPlayerReady() || !flow::InSharedWorld()) {
        error = TR("Передать деньги можно, когда вы в игре.", "You can give money once you are in the game.");
    } else if (amount <= 0) {
        error = TR("Введите сумму.", "Enter an amount.");
    } else if (amount > Mine()) {
        error = Format(TR("У вас только $%d.", "You have only $%d."), std::max(Mine(), 0));
    } else if (Of(to, theirs) && (int64_t)theirs + amount > kMaxMoney) {
        error = Format(TR("У игрока %s не поместится столько: больше $%d не бывает.", "%s cannot hold that much: $%d is the most."),
                       Net().PeerName(to).c_str(), kMaxMoney);
    } else {
        Mine() -= amount;
        g_unsure[to] += amount;
        Send(OP_GIVE, to, amount);
        ui::TheChat().AddSystem(Format(TR("Вы передали $%d игроку %s.", "You gave $%d to %s."), amount, Net().PeerName(to).c_str()));
        LOGI("Money to player %d: $%d", to, amount);
        if (!Net().IsHost()) SaveProfileNow();
        return true;
    }
    return false;
}

}  // namespace coop::money
