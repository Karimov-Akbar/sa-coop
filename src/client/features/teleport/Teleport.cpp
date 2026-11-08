// /tp and /tphere as requests (29.10: the one asked says yes first). The other player sees who asks and what in the middle
// of its screen and answers with Y or N within 20 s; the asker's game then does what the command did at once before
// (Players: /tp goes over to that player, /tphere brings it over), with the same rules about buildings. The host may
// forbid both to its partners (F9, creating the game). With three or four players the command takes the other player's
// number, as nicknames are awkward to type (the user's word of 06.11): the F2 window and /players show it.
#include "features/teleport/Teleport.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"

namespace coop::teleport {

namespace {
enum : uint8_t { OP_GO = 1, OP_BRING = 2, OP_YES = 3, OP_NO = 4 };
constexpr uint64_t kAnswerMs = 20000;

struct Request {
    uint8_t who = 0xFF;  // the other player
    bool bring = false;  // /tphere (else /tp)
    uint64_t until = 0;
};
std::vector<Request> g_asked;  // ours, each until its answer
std::deque<Request> g_asking;  // the others', the first one shown until our answer: two may ask at once

auto AskedOf(uint8_t who) {
    return std::find_if(g_asked.begin(), g_asked.end(), [who](const Request& q) { return q.who == who; });
}
void DropStale() {
    while (!g_asking.empty() && (NowMs() >= g_asking.front().until || !Net().Peers().count(g_asking.front().who))) g_asking.pop_front();
}

void Send(uint8_t op, uint8_t to) {
    Writer w(proto::G_TP_ASK);
    w.U8(op);
    w.U8(to);
    Net().Send(proto::CH_RELIABLE, w);
}

void Handle(uint8_t from, Reader& r) {
    uint8_t op = r.U8(), to = r.U8();
    if (!r.Ok() || to != Net().MyId() || !Net().Peers().count(from)) return;
    std::string name = Net().PeerName(from);
    if (op == OP_GO || op == OP_BRING) {
        // our CJ is parked out of the world by a mission (ZERO4's RC part): coming over dropped the partner into the bay
        // with 4 stars of a closed city (30-31.10). Our game says no by itself.
        if (op == OP_GO && OwnCarFrozen()) {
            Send(OP_NO, from);
            LOGI("Teleport request from %s: no, our CJ is held in a frozen vehicle", name.c_str());
            return;
        }
        std::erase_if(g_asking, [from](const Request& q) { return q.who == from; });  // its newer word
        g_asking.push_back({from, op == OP_BRING, NowMs() + kAnswerMs});
        ui::TheChat().AddSystem(Format(op == OP_GO ? TR("%s просит разрешения переместиться к вам.", "%s asks to come over to you.")
                                                   : TR("%s зовёт вас к себе.", "%s asks you to come over."),
                                       name.c_str()));
        LOGI("Teleport request from %s (%s)", name.c_str(), op == OP_GO ? "/tp" : "/tphere");
        return;
    }
    auto asked = AskedOf(from);
    if (asked == g_asked.end() || (op != OP_YES && op != OP_NO)) return;
    Request q = *asked;
    g_asked.erase(asked);
    if (op == OP_NO) {
        ui::TheChat().AddSystem(Format(TR("%s отказывает в перемещении.", "%s says no."), name.c_str()));
        return;
    }
    LOGI("Teleport request to %s: yes", name.c_str());
    const RemotePlayer* rp = Players::Instance().Get(q.who);
    if (!rp || !rp->hasState || !core::IsPlayerReady() || !Players::Instance().TeleportAllowed(*rp, q.bring)) return;
    if (q.bring) Players::Instance().Bring(q.who);
    else Players::Instance().GoToPlayer(*rp);
}

class Teleport : public Module {
public:
    const char* Name() const override { return "Teleport"; }
    void OnInstallHooks() override { Net().SetHandler(proto::G_TP_ASK, &Handle); }
    void OnSessionEnd() override { g_asked.clear(), g_asking.clear(); }
    void OnPeerLeave(uint8_t id) override {
        std::erase_if(g_asked, [id](const Request& q) { return q.who == id; });
        std::erase_if(g_asking, [id](const Request& q) { return q.who == id; });
    }
    void OnPostProcess() override {
        for (auto it = g_asked.begin(); it != g_asked.end();) {
            if (NowMs() < it->until) {
                ++it;
                continue;
            }
            ui::TheChat().AddSystem(Format(TR("Нет ответа на запрос от игрока %s.", "No answer to the request from %s."),
                                           Net().PeerName(it->who).c_str()));
            it = g_asked.erase(it);
        }
    }
    bool OnCommand(const std::string& cmd, const std::vector<std::string>& args) override { return Command(cmd, args); }

    static Teleport& Instance() {
        static Teleport t;
        return t;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Teleport::Instance()); }
} g_reg;
}  // namespace

bool Command(const std::string& cmd, const std::vector<std::string>& args) {
    if ((cmd != "tp" && cmd != "tphere") || !Net().Online()) return false;
    if (!core::IsPlayerReady()) return true;
    if (!Net().IsHost() && !Net().PartnerTeleport()) {
        ui::TheChat().AddError(TR("Хост запретил напарникам команды /tp и /tphere.", "The host turned /tp and /tphere off for partners."));
        return true;
    }
    // whom: its number (or nickname), the only other player with none given
    uint8_t who = 0xFF;
    for (auto& [id, peer] : Net().Peers())
        if (args.empty() ? Net().Peers().size() == 1 : args[0] == std::to_string(id) || !_stricmp(peer.name.c_str(), args[0].c_str())) who = id;
    if (who == 0xFF && !args.empty()) {
        ui::TheChat().AddError(Format(TR("Игрока %s нет в игре.", "There is no player %s in the game."), args[0].c_str()));
        return true;
    }
    if (who == 0xFF && Net().Peers().size() > 1) {
        std::string list;
        for (auto& [id, peer] : Net().Peers()) list += Format("%s/%s %d (%s)", list.empty() ? "" : ", ", cmd.c_str(), id, peer.name.c_str());
        ui::TheChat().AddSystem(Format(TR("Кого? Напишите номер игрока: %s.", "Whom? Type the player's number: %s."), list.c_str()));
        return true;
    }
    const RemotePlayer* target = Players::Instance().Get(who);
    if (!target || !target->hasState) {
        ui::TheChat().AddError(TR("Напарник ещё не в игре.", "Your partner is not in the game yet."));
        return true;
    }
    bool bring = cmd == "tphere";
    if (!Players::Instance().TeleportAllowed(*target, bring)) return true;
    std::string name = Net().PeerName(target->id);
    if (AskedOf(target->id) != g_asked.end()) {
        ui::TheChat().AddError(Format(TR("Запрос игроку %s уже отправлен. Ждём ответа.", "The request to %s is sent. Waiting for the answer."),
                                      name.c_str()));
        return true;
    }
    Send(bring ? OP_BRING : OP_GO, target->id);
    g_asked.push_back({target->id, bring, NowMs() + kAnswerMs});
    ui::TheChat().AddSystem(Format(TR("Запрос отправлен игроку %s.", "Request sent to %s."), name.c_str()));
    LOGI("Teleport request to %s (%s)", name.c_str(), cmd.c_str());
    return true;
}

bool Key(bool yes) {
    DropStale();
    if (g_asking.empty()) return false;
    Send(yes ? OP_YES : OP_NO, g_asking.front().who);
    g_asking.pop_front();
    return true;
}

std::string Text(std::string& hint) {
    DropStale();
    if (g_asking.empty()) return "";
    hint = TR("Y - да, N - нет", "Y: yes, N: no");
    return Format(g_asking.front().bring ? TR("%s зовёт вас к себе", "%s asks you over") : TR("%s просит переместиться к вам", "%s asks to come over"),
                  Net().PeerName(g_asking.front().who).c_str());
}

}  // namespace coop::teleport
