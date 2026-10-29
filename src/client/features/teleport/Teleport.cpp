// /tp and /tphere as requests (29.10: the one asked says yes first). The other player sees who asks and what in the middle
// of its screen and answers with Y or N within 20 s; the asker's game then does what the command did at once before
// (Players: /tp goes over to that player, /tphere brings it over), with the same rules about buildings. The host may
// forbid both to its partners (F9, creating the game).
#include "features/teleport/Teleport.h"

#include <cstring>
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
Request g_asked;   // ours, until its answer
Request g_asking;  // the other player's, until our answer

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
        g_asking = {from, op == OP_BRING, NowMs() + kAnswerMs};
        ui::TheChat().AddSystem(Format(op == OP_GO ? TR("%s просит разрешения переместиться к вам.", "%s asks to come over to you.")
                                                   : TR("%s зовёт вас к себе.", "%s asks you to come over."),
                                       name.c_str()));
        LOGI("Teleport request from %s (%s)", name.c_str(), op == OP_GO ? "/tp" : "/tphere");
        return;
    }
    if (from != g_asked.who || (op != OP_YES && op != OP_NO)) return;
    Request q = g_asked;
    g_asked = {};
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
    void OnSessionEnd() override { g_asked = g_asking = {}; }
    void OnPeerLeave(uint8_t id) override {
        if (g_asked.who == id) g_asked = {};
        if (g_asking.who == id) g_asking = {};
    }
    void OnPostProcess() override {
        if (g_asked.who == 0xFF || NowMs() < g_asked.until) return;
        ui::TheChat().AddSystem(Format(TR("Нет ответа на запрос от игрока %s.", "No answer to the request from %s."),
                                       Net().PeerName(g_asked.who).c_str()));
        g_asked = {};
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
    // whom: the one named, else the only other player (the host, for one of several partners)
    const RemotePlayer* target = nullptr;
    for (auto& [id, rp] : Players::Instance().All()) {
        bool named = !args.empty() && (!_stricmp(Net().PeerName(id).c_str(), args[0].c_str()) || args[0] == std::to_string(id));
        if (named || (args.empty() && (Players::Instance().All().size() == 1 || id == Net().HostId()))) target = &rp;
    }
    if (!target && !args.empty()) {
        ui::TheChat().AddError(Format(TR("Игрока %s нет в игре.", "There is no player %s in the game."), args[0].c_str()));
        return true;
    }
    if (!target && Players::Instance().All().size() > 1) {
        ui::TheChat().AddError(Format(TR("Кого? Напишите /%s и ник.", "Whom? Type /%s and a nickname."), cmd.c_str()));
        return true;
    }
    if (!target || !target->hasState) {
        ui::TheChat().AddError(TR("Напарник ещё не в игре.", "Your partner is not in the game yet."));
        return true;
    }
    bool bring = cmd == "tphere";
    if (!Players::Instance().TeleportAllowed(*target, bring)) return true;
    std::string name = Net().PeerName(target->id);
    if (g_asked.who == target->id && NowMs() < g_asked.until) {
        ui::TheChat().AddError(Format(TR("Запрос игроку %s уже отправлен. Ждём ответа.", "The request to %s is sent. Waiting for the answer."),
                                      name.c_str()));
        return true;
    }
    Send(bring ? OP_BRING : OP_GO, target->id);
    g_asked = {target->id, bring, NowMs() + kAnswerMs};
    ui::TheChat().AddSystem(Format(TR("Запрос отправлен игроку %s.", "Request sent to %s."), name.c_str()));
    LOGI("Teleport request to %s (%s)", name.c_str(), cmd.c_str());
    return true;
}

bool Key(bool yes) {
    if (g_asking.who == 0xFF || NowMs() >= g_asking.until) return false;
    uint8_t who = g_asking.who;
    g_asking = {};
    if (Net().Peers().count(who)) Send(yes ? OP_YES : OP_NO, who);
    return true;
}

std::string Text(std::string& hint) {
    if (g_asking.who == 0xFF || NowMs() >= g_asking.until || !Net().Peers().count(g_asking.who)) return "";
    hint = TR("Y - да, N - нет", "Y: yes, N: no");
    return Format(g_asking.bring ? TR("%s зовёт вас к себе", "%s asks you over") : TR("%s просит переместиться к вам", "%s asks to come over"),
                  Net().PeerName(g_asking.who).c_str());
}

}  // namespace coop::teleport
