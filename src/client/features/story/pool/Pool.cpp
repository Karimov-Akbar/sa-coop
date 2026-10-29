// Pool for two over the network (see Pool.h). 27.10: a pool contest had each player at a table of its own, and the
// second one never came to the first one's. The second player's turns run in the owner's POOL2 from its buttons; the
// owner's game sends it the camera, the opponent's place and animation (the second player's own), and the balls.
#include "features/story/pool/Pool.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "entities/vehicle/VehicleState.h"
#include "features/story/contest/Contest.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Anim.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"

namespace coop::pool {

namespace {
using namespace game;
using game::script::Cmd;
using game::script::NextArg;
using game::script::OutVar;

enum : uint8_t { OP_START = 1, OP_END = 2, OP_FAIL = 3, OP_VIEW = 4, OP_OBJECTS = 5, OP_INPUT = 6, OP_RESULT = 7, OP_TABLE = 8 };
constexpr uintptr_t kMissionLocals = 0xA48960;   // CTheScripts::LocalVariablesForCurrentMission
constexpr uintptr_t kMissionCleanUp = 0xA90850;  // CTheScripts::MissionCleanUp: 75 x {u8 type, i32 handle}
constexpr uintptr_t kScriptParams = 0xA43C78;    // CTheScripts::ScriptParams: the last command's arguments
constexpr uintptr_t kMouse = 0xB73418;           // CPad::NewMouseControllerState: the move at +0x0C, +0x10
// POOL2's (mission 8) locals: our cue, the opponent's, the opponent (the second player's place), the mode for two, whose
// pad it is
constexpr int kOurCue = 106, kTheirCue = 107, kOpponent = 214, kTwoPlayers = 178, kTurn = 180, kTable = 163;
constexpr uint64_t kAskMs = 3000, kViewMs = 50, kQuietMs = 10000, kGlideMs = 60, kTableMs = 1000, kTableQuietMs = 3000;

int32_t Local(int n) { return Ref<int32_t>(kMissionLocals + (uintptr_t)n * 4); }
void* Pool2() {
    void* s = RunningMission();
    return s && !_strnicmp(Field<char[8]>(s, 0x08), "pool2", 8) ? s : nullptr;
}
bool TableModel(int model) { return model == 2964 || model == 3094; }  // k_pooltablesm, k_pooltableb
std::string Name(uint8_t id) { return Net().PeerName(id); }
void Send(uint8_t op, uint8_t to, uint8_t value = 0) {
    Writer w(proto::G_POOL);
    w.U8(op), w.U8(to), w.U8(value);
    Net().Send(proto::CH_RELIABLE, w);
}
// a pad state's first 20 values are in the order of the buttons' ids (0 the left stick's x .. 19 the right shock)
int16_t Button(const CControllerState& pad, int b) {
    int16_t v[24];
    memcpy(v, &pad, sizeof v);
    return b >= 0 && b < 20 ? v[b] : 0;
}
// ---------------------------------------------------------------- the owner: the table is ours
struct Owner {
    uint8_t with = 0xFF;      // the second player
    uint64_t askUntil = 0;    // asked of the bet screen, not on yet
    bool on = false;
    bool alone = false;       // it left mid-game: our own pad plays its turns (POOL2 waited for the second pad for good)
    bool ballTold = false;    // a ball of ours logged once a game, to compare with the second player's
    CControllerState pad{};   // its buttons and sticks
    bool joypad = false;
    float mx = 0.0f, my = 0.0f;  // its mouse since the game last read it
    int turn = 0;
    int opponent = -1;        // the bar's opponent, hidden while the copy stands in its place
    int cue = -1;             // its cue, in the copy's hand meanwhile (HandCue)
    uint64_t sentMs = 0;
} g_own;

// The opponent's cue in our copy's hand. Held by the hidden opponent, it hid with it (CTaskSimpleHoldEntity::
// SetPedPosition shows a held thing as its holder) in both games: the second player aimed and struck with empty hands
// (28.10). POOL2 still plays the cue's own animation in step with the hidden one's, and asks whether that one holds it
// (Answer says so).
void HandCue() {
    const RemotePlayer* rp = Players::Instance().Get(g_own.with);
    int cue = Local(kTheirCue);
    if (!rp || !rp->ped || rp->ped->InVehicle() || !GetObject(cue)) return;
    if (Cmd(0x0737).I(g_own.opponent).I(cue).Run()) Cmd(0x070B).I(g_own.opponent).I(0).Run();  // IS_CHAR_HOLDING_OBJECT, DROP_OBJECT
    Cmd(0x06AB).I(rp->pedHandle).I(1).Run();  // HIDE_CHAR_WEAPON_FOR_SCRIPTED_CUTSCENE, as POOL2 hides both players' weapons
    // TASK_PICK_UP_OBJECT as POOL2 gives it: into the right hand, no animation
    if (!Cmd(0x0737).I(rp->pedHandle).I(cue).Run())
        Cmd(0x070A).I(rp->pedHandle).I(cue).F(0.0f).F(0.0f).F(0.0f).I(6).I(16).S("NULL").S("NULL").I(1).Run();
    g_own.cue = cue;
}

// the cue back to the bar's opponent: POOL2 picks it up again once told the truth
void CueBack() {
    const RemotePlayer* rp = g_own.cue >= 0 && g_own.with != 0xFF ? Players::Instance().Get(g_own.with) : nullptr;
    if (rp && rp->ped && GetObject(g_own.cue) && Cmd(0x0737).I(rp->pedHandle).I(g_own.cue).Run())
        Cmd(0x070B).I(rp->pedHandle).I(0).Run();  // DROP_OBJECT
    if (rp && rp->ped) Cmd(0x06AB).I(rp->pedHandle).I(0).Run();  // its weapon shown again
    g_own.cue = -1;
}

void OwnerStart() {
    g_own.on = true, g_own.askUntil = 0;
    Send(OP_START, g_own.with);
    contest::Drop();  // played together now: the game's winner is the result
    ui::TheChat().AddSystem(Format(TR("Партия вдвоём: %s играет вторым, удары по очереди.", "A game for two: %s plays second, turn by turn."),
                                   Name(g_own.with).c_str()));
    LOGI("Pool for two with %s at our table", Name(g_own.with).c_str());
}

void OwnerEnd() {
    if (CPed* npc = GetPed(g_own.opponent)) npc->Flags() |= 0x80u;  // CEntity::m_bIsVisible
    CueBack();
    if (g_own.with != 0xFF) Players::Instance().SetAvatar(g_own.with, -1);
    if (!g_own.on) {
        Send(OP_FAIL, g_own.with, Pool2() ? 0 : 1);  // the bet screen was behind us (our game had begun alone), or we left it
        LOGI("Pool for two: our game began alone or the table was left");
    } else if (!g_own.alone) {
        Send(OP_END, g_own.with);
        LOGI("Pool for two over");
    }
    g_own = {};
}

// the second player gone mid-game: its turns are ours, and the bar's opponent stands there again
void OwnerAlone() {
    if (CPed* npc = GetPed(g_own.opponent)) npc->Flags() |= 0x80u;
    CueBack();
    g_own.alone = true;
    ui::TheChat().AddSystem(Format(TR("%s нет в игре: удары второго игрока теперь ваши.", "%s is gone: the second player's shots are yours now."),
                                   Name(g_own.with).c_str()));
}

void Won(uint8_t second) {
    Send(OP_RESULT, g_own.with, second);
    ui::TheChat().AddSystem(second ? Format(TR("Партия: побеждает %s.", "The game: %s wins."), Name(g_own.with).c_str())
                                   : TR("Партия: побеждаете вы.", "The game: you win."));
    LOGI("Pool for two: %s won", second ? "the second player" : "we");
}

void SendView(CPed* npc) {
    uintptr_t c = addr::TheCamera;
    CVector src, front;
    float fov;
    if (Ref<uint8_t>(c + 0x58)) {  // m_bTransitionState: the drawn pose is interpolated in CCamera, not in the cam
        src = Ref<CVector>(c + 0x9A4), front = Ref<CVector>(c + 0x984), fov = Ref<float>(c + 0xEC);
    } else {
        src = cam::Source(), front = cam::Front(), fov = cam::Fov();
    }
    CVector at = npc->GetPosition();
    void* a = anim::Main(npc->RwObject());
    int block = -1, idx = -1;
    if (a) anim::Locate(anim::Hierarchy(a), block, idx);
    // the opponent's cue's own animation (POOL_*_O, in step with its holder's): the second player's hand holds its mirror
    CObject* cue = GetObject(Local(kTheirCue));
    auto* cc = cue ? (uint8_t*)cue->RwObject() : nullptr;
    void* ca = cc && cc[0] == 2 && Call<0x4D6760, bool>(cc) ? anim::Main(cc) : nullptr;  // an RpClump, RpAnimBlendClumpIsInitialized
    int cueBlock = -1, cueIdx = -1;
    if (ca) anim::Locate(anim::Hierarchy(ca), cueBlock, cueIdx);
    Writer w(proto::G_POOL);
    w.U8(OP_VIEW), w.U8(g_own.with);
    w.Vec({src.x, src.y, src.z}), w.Vec({front.x, front.y, front.z}), w.F32(fov);
    w.Vec({at.x, at.y, at.z}), w.F32(npc->CurrentRotation());
    w.I16((int16_t)block), w.I16((int16_t)idx);
    w.F32(a ? anim::CurrentTime(a) : 0.0f), w.F32(a ? Field<float>(a, 0x24) : 1.0f), w.U16(a ? anim::Flags(a) : 0);
    w.U8((uint8_t)g_own.turn);
    w.I8(npc->AreaCode());
    w.I16((int16_t)cueBlock), w.I16((int16_t)cueIdx), w.F32(ca ? anim::CurrentTime(ca) : 0.0f), w.U16(ca ? anim::Flags(ca) : 0);
    Net().Send(proto::CH_STATE, w);
}

// the game's objects (the balls, the opponent's cue, the aiming one): ours is in our copy's hand there (Props), the
// opponent's in the second player's own hand there
void SendObjects() {
    struct Entry {
        uint8_t slot;
        uint16_t model;
        CVector pos;
        float q[4];
        uint8_t look;  // 1: shown, 2: held there
    };
    std::vector<Entry> list;
    int ourCue = Local(kOurCue), theirCue = Local(kTheirCue);
    for (int i = 0; i < 75; ++i) {
        uintptr_t e = kMissionCleanUp + (uintptr_t)i * 8;
        int h = Ref<int32_t>(e + 4);
        CObject* o = Ref<uint8_t>(e) == 3 ? GetObject(h) : nullptr;  // MISSION_CLEANUP_ENTITY_TYPE_OBJECT
        if (!o || h == ourCue) continue;
        // shown as here: one with no model in place is not seen
        Entry x{(uint8_t)i, (uint16_t)o->ModelIndex(), o->GetPosition(), {0, 0, 0, 1},
                (uint8_t)((o->IsVisible() && o->RwObject() ? 1 : 0) | (h == theirCue ? 2 : 0))};
        // its matrix (CPlaceable), as the other copies of objects go (Props, Objects). GET_OBJECT_QUATERNION read the
        // model's (CEntity::GetModellingMatrix), none while no model is in place: the owner's game crashed at the table's
        // start (0x0059C3E4)
        if (CMatrix* mx = o->Matrix()) {
            Quat r = Quat::FromMatrix(*mx);
            x.q[0] = r.x, x.q[1] = r.y, x.q[2] = r.z, x.q[3] = r.w;
        }
        list.push_back(x);
        if (!g_own.ballTold && h != theirCue && (x.look & 1)) {
            g_own.ballTold = true;
            LOGI("Pool for two: our object %d (model %d) at %.2f,%.2f,%.2f, area %d", i, (int)x.model, x.pos.x, x.pos.y, x.pos.z,
                 (int)o->AreaCode());
        }
    }
    Writer w(proto::G_POOL);
    w.U8(OP_OBJECTS), w.U8(g_own.with);
    w.U8((uint8_t)list.size());
    for (const Entry& x : list) {
        w.U8(x.slot), w.U16(x.model), w.Vec({x.pos.x, x.pos.y, x.pos.z});
        for (float f : x.q) w.F32(f);
        w.U8(x.look);
    }
    Net().Send(proto::CH_STATE, w);
}

void OwnerUpdate() {
    if (g_own.with == 0xFF) return;
    if (!Pool2() || (g_own.on && Local(kTwoPlayers) != 1)) return OwnerEnd();  // the game is over
    if (!g_own.on) {
        if (Local(kTwoPlayers) == 1) OwnerStart();
        else if (NowMs() > g_own.askUntil) OwnerEnd();
        return;
    }
    int turn = Local(kTurn);
    if (turn != g_own.turn) g_own.mx = g_own.my = 0.0f;  // the mouse moved before its turn aims nothing
    g_own.turn = turn;
    if (g_own.alone) return;
    g_own.opponent = Local(kOpponent);
    CPed* npc = GetPed(g_own.opponent);
    if (!npc) return;
    npc->Flags() &= ~0x80u;  // the copy stands there instead
    Players::Instance().SetAvatar(g_own.with, g_own.opponent);
    HandCue();
    if (NowMs() - g_own.sentMs < kViewMs) return;
    g_own.sentMs = NowMs();
    SendView(npc);
    SendObjects();
}

void ReadInput(Reader& r) {
    int16_t v[24] = {};
    for (int i = 0; i < 20; ++i) v[i] = r.I16();
    bool joypad = r.U8() != 0;
    float mx = r.F32(), my = r.F32();
    if (!r.Ok() || !std::isfinite(mx) || !std::isfinite(my)) return;
    memcpy(&g_own.pad, v, sizeof v);
    g_own.joypad = joypad;
    if (g_own.turn == 1) g_own.mx += std::clamp(mx, -200.0f, 200.0f), g_own.my += std::clamp(my, -200.0f, 200.0f);
}

// ---------------------------------------------------------------- the second player: the table is the owner's
struct Mirror {
    int handle = -1;
    uint16_t model = 0;
    CVector from, to;  // a view moves it from where it was shown to where it is there, in kGlideMs
    float qFrom[4] = {0, 0, 0, 1}, qTo[4] = {0, 0, 0, 1};
    uint64_t ms = 0;
};
struct Joined {
    uint8_t from = 0xFF;
    uint64_t viewMs = 0;      // the owner's last view came then
    CVector src, front;       // the owner's camera
    float fov = 70.0f;
    bool camOn = false;
    CVector shownSrc, shownFront;
    float shownFov = 70.0f;
    uint64_t shownMs = 0;
    CVector at, start;        // the opponent's place now and at our first view (where our bar's own opponent waits)
    float heading = 0.0f;
    int16_t block = -1, idx = -1;
    float time = 0.0f, speed = 1.0f;
    uint16_t flags = 0;
    int8_t area = 0;
    bool placed = false;      // brought to the owner's table (its interior's door used as a door)
    void* applied = nullptr;  // the animation our player plays of the opponent's
    int16_t cueBlock = -1, cueIdx = -1;  // the opponent's cue's own animation there, at its time
    float cueTime = 0.0f;
    uint16_t cueFlags = 0;
    int cueSlot = -1;                    // its mirror's: in our player's hand here
    void* cueApplied = nullptr;
    std::map<uint8_t, Mirror> mirrors;  // the owner's objects by their cleanup slot
    std::vector<CPed*> hidden;          // our bar's own opponent at the table
    uint64_t hideMs = 0;
    int told = -1;                      // the objects shown and hidden last logged (shown << 8 | hidden)
    bool ballTold = false;              // a ball's state here logged once a game (29.10: made, shown and not seen)
} g_join;

void DeleteMirror(Mirror& m) {
    if (m.handle >= 0 && GetObject(m.handle)) Cmd(0x0108).I(m.handle).Run();  // DELETE_OBJECT
    m.handle = -1;
}

// the mirror of the opponent's cue, in our player's hand: -1 while there is none
int CueMirror() {
    auto it = g_join.mirrors.find((uint8_t)g_join.cueSlot);
    return g_join.cueSlot >= 0 && it != g_join.mirrors.end() && it->second.handle >= 0 && GetObject(it->second.handle) ? it->second.handle : -1;
}

void StopCamera() {
    if (!g_join.camOn) return;
    g_join.camOn = false;
    Cmd(0x0925).Run();  // CAMERA_RESET_NEW_SCRIPTABLES
    Cmd(0x02EB).Run();  // RESTORE_CAMERA_JUMPCUT
    Cmd(0x0373).Run();  // SET_CAMERA_BEHIND_PLAYER
}

void JoinStart(uint8_t from) {
    g_join = {};
    g_join.from = from;
    g_join.viewMs = NowMs();
    contest::Drop();  // played together now
    Cmd(0x01B4).I(0).I(0).Run();  // SET_PLAYER_CONTROL off: our buttons play at the owner's table
    Cmd(0x0581).I(0).Run();       // DISPLAY_RADAR off, as at a table of our own
    Cmd(0x0826).I(0).Run();       // DISPLAY_HUD
    if (CPlayerPed* me = FindPlayerPed(0)) {
        Cmd(0x04D7).I(GetPedRef(me)).I(1).Run();  // FREEZE_CHAR_POSITION: placed by the view
        Cmd(0x06AB).I(GetPedRef(me)).I(1).Run();  // HIDE_CHAR_WEAPON_FOR_SCRIPTED_CUTSCENE: the cue in hand, as POOL2 has it
    }
    ui::TheChat().AddSystem(Format(TR("Партия вдвоём за столом игрока %s: вы играете вторым, удары по очереди.",
                                      "A game for two at %s's table: you play second, turn by turn."),
                                   Name(from).c_str()));
    LOGI("Pool for two at %s's table", Name(from).c_str());
}

void LetGo(CPlayerPed* me);

void JoinEnd() {
    if (g_join.from == 0xFF) return;
    if (CPlayerPed* me = FindPlayerPed(0); me && CueMirror() >= 0 && Cmd(0x0737).I(GetPedRef(me)).I(CueMirror()).Run())
        Cmd(0x070B).I(GetPedRef(me)).I(0).Run();  // IS_CHAR_HOLDING_OBJECT, DROP_OBJECT: the cue out of our hand before it goes
    for (auto& [slot, m] : g_join.mirrors) DeleteMirror(m);
    for (CPed* p : g_join.hidden)
        if (IsPedValid(p)) p->Flags() |= 0x80u;
    StopCamera();
    if (CPlayerPed* me = FindPlayerPed(0)) {
        if (void* clump = me->RwObject())
            for (void* a = anim::First(clump); a; a = anim::Next(a))
                if (anim::Hierarchy(a) == g_join.applied && anim::BlendDelta(a) >= 0.0f) anim::FadeOut(a);
        LetGo(me);  // frozen until it stands on the floor
        Cmd(0x06AB).I(GetPedRef(me)).I(0).Run();  // HIDE_CHAR_WEAPON_FOR_SCRIPTED_CUTSCENE off
        Cmd(0x0792).I(GetPedRef(me)).Run();       // CLEAR_CHAR_TASKS_IMMEDIATELY: its own stance back
    }
    Cmd(0x01B4).I(0).I(1).Run();  // SET_PLAYER_CONTROL
    Cmd(0x0581).I(1).Run();       // DISPLAY_RADAR
    Cmd(0x0826).I(1).Run();       // DISPLAY_HUD
    LOGI("Pool for two at %s's table: over", Name(g_join.from).c_str());
    g_join = {};
}

void ReadView(Reader& r) {
    Vec3 s = r.Vec(), f = r.Vec();
    float fov = r.F32();
    Vec3 at = r.Vec();
    float heading = r.F32();
    int16_t block = r.I16(), idx = r.I16();
    float time = r.F32(), speed = r.F32();
    uint16_t flags = r.U16();
    r.U8();  // whose turn: the owner's game decides it
    int8_t area = r.I8();
    int16_t cueBlock = r.I16(), cueIdx = r.I16();
    float cueTime = r.F32();
    uint16_t cueFlags = r.U16();
    CVector front{f.x, f.y, f.z};
    float fl = front.Length();
    if (!r.Ok() || !std::isfinite(s.x) || !std::isfinite(at.x) || !(fl > 0.5f && fl < 1.5f) || !(fov > 5.0f && fov < 170.0f) ||
        !std::isfinite(time) || !std::isfinite(speed) || !std::isfinite(cueTime))
        return;
    g_join.src = {s.x, s.y, s.z}, g_join.front = front, g_join.fov = fov;
    g_join.at = {at.x, at.y, at.z}, g_join.heading = heading;
    g_join.block = block, g_join.idx = idx, g_join.time = time, g_join.speed = std::clamp(speed, 0.0f, 10.0f), g_join.flags = flags;
    g_join.cueBlock = cueBlock, g_join.cueIdx = cueIdx, g_join.cueTime = cueTime, g_join.cueFlags = cueFlags;
    g_join.area = area;
    g_join.viewMs = NowMs();
    if (!g_join.placed) {  // into the owner's bar, by its door
        if (const RemotePlayer* owner = Players::Instance().Get(g_join.from)) Players::Instance().GoTo(*owner, g_join.at, true);
        g_join.start = g_join.at;
        g_join.placed = true;
    }
    if (!g_join.camOn) {
        CVector look = g_join.src + g_join.front * 10.0f;
        Cmd(0x015F).F(g_join.src.x).F(g_join.src.y).F(g_join.src.z).F(0.0f).F(0.0f).F(0.0f).Run();  // SET_FIXED_CAMERA_POSITION
        Cmd(0x0160).F(look.x).F(look.y).F(look.z).I(2).Run();  // POINT_CAMERA_AT_POINT, a jump cut: the script's camera
        Cmd(0x0925).Run();                                     // CAMERA_RESET_NEW_SCRIPTABLES
        Cmd(0x0930).I(1).Run();                                // CAMERA_PERSIST_POS
        Cmd(0x092F).I(1).Run();                                // CAMERA_PERSIST_TRACK
        Cmd(0x0931).I(1).Run();                                // CAMERA_PERSIST_FOV
        g_join.shownSrc = g_join.src, g_join.shownFront = g_join.front, g_join.shownFov = g_join.fov, g_join.shownMs = NowMs();
        g_join.camOn = true;
    }
}

// A view comes each 50 ms or so, and a ball rolled up to half a metre in between: shown there at once, the balls jumped
// across the table (28.10). Each glides from where it is shown to its newest place in kGlideMs.
float Glide(const Mirror& m, uint64_t now) { return std::clamp((float)(now - m.ms) / (float)kGlideMs, 0.0f, 1.0f); }
void Shown(const Mirror& m, float k, CVector& at, float q[4]) {
    at = m.from + (m.to - m.from) * k;
    float dot = 0.0f, len = 0.0f;
    for (int i = 0; i < 4; ++i) dot += m.qFrom[i] * m.qTo[i];
    float s = dot < 0.0f ? -1.0f : 1.0f;  // the shorter way round
    for (int i = 0; i < 4; ++i) q[i] = m.qFrom[i] + (s * m.qTo[i] - m.qFrom[i]) * k, len += q[i] * q[i];
    len = std::sqrt(len);
    for (int i = 0; i < 4; ++i) q[i] = len > 1e-6f ? q[i] / len : m.qTo[i];
}
// As the other copies of objects go (Props, Objects): out of the world, the matrix, back in. Turned by
// SET_OBJECT_QUATERNION and moved with no new place in the world's lists, the balls were made and "shown" here but never
// seen at the owner's table (28.10, three tests).
void Place(CObject* o, const CVector& at, const float q[4]) {
    CMatrix* mx = o->Matrix();
    if (!mx) return;
    WorldRemove(o);
    Quat{q[0], q[1], q[2], q[3]}.ToMatrix(*mx);
    mx->pos = at;               // no offset to its base
    CallMethod<0x446F90>(o);    // CEntity::UpdateRW
    CallMethod<0x532B00>(o);    // CEntity::UpdateRwFrame
    WorldAdd(o);
}
bool Still(const Mirror& m) { return (m.to - m.from).Length() < 0.001f && !memcmp(m.qFrom, m.qTo, sizeof m.qTo); }

void MoveMirrors() {
    uint64_t now = NowMs();
    for (auto& [slot, m] : g_join.mirrors) {
        CObject* o = m.handle >= 0 ? GetObject(m.handle) : nullptr;
        if (!o || slot == g_join.cueSlot || Still(m) || now - m.ms > kGlideMs + 100) continue;  // placed already, or in our hand
        CVector at;
        float q[4];
        Shown(m, Glide(m, now), at, q);
        Place(o, at, q);
    }
}

void ReadObjects(Reader& r) {
    uint8_t n = r.U8();
    std::set<uint8_t> seen;
    for (int k = 0; k < n; ++k) {
        uint8_t slot = r.U8();
        uint16_t model = r.U16();
        Vec3 p = r.Vec();
        float q[4];
        for (float& f : q) f = r.F32();
        uint8_t look = r.U8();
        bool visible = look & 1, held = look & 2;  // held: the opponent's cue, in our hand (HoldCue)
        if (!r.Ok() || !std::isfinite(p.x) || !std::isfinite(q[0])) return;
        if (held) g_join.cueSlot = slot;
        seen.insert(slot);
        Mirror& m = g_join.mirrors[slot];
        if (m.handle >= 0 && (m.model != model || !GetObject(m.handle))) DeleteMirror(m);
        bool made = false;
        if (m.handle < 0) {
            if (!IsValidModel(model)) continue;
            if (!IsModelLoaded(model)) {
                // in the background, the next view makes it. With no flag the request lasted one streaming update
                // (CStreaming::PurgeRequestList): no balls for the first 20 s of a game (28.10)
                RequestModel(model, 0x10 /* PRIORITY_REQUEST */);
                continue;
            }
            Cmd(0x0107).I(model).F(p.x).F(p.y).F(p.z).Out(&m.handle).Run();  // CREATE_OBJECT
            if (m.handle < 0) continue;
            m.model = model;
            Cmd(0x0382).I(m.handle).I(0).Run();  // SET_OBJECT_COLLISION off: nothing here rolls it
            Cmd(0x0550).I(m.handle).I(1).Run();  // FREEZE_OBJECT_POSITION
            made = true;
        }
        CObject* o = GetObject(m.handle);
        if (!o) continue;
        if (!o->RwObject()) CallMethod<0x533D30>(o);  // CEntity::CreateRwObject: nothing is drawn of one with no model in place
        // in the area our player is in, at the owner's table: the opponent's area the view brings can be the street's (a
        // script's person keeps area 0), and the balls were made, shown and never seen in the bar (28.10, twice)
        if (int area = Ref<int32_t>(0xB72914); o->AreaCode() != area) Cmd(0x0566).I(m.handle).I(area).Run();  // CGame::currArea, SET_OBJECT_AREA_VISIBLE
        if (held) continue;  // our hand places and shows it
        uint64_t now = NowMs();
        CVector at, to{p.x, p.y, p.z};
        float shown[4];
        Shown(m, Glide(m, now), at, shown);
        bool jump = made || (to - at).Length() > 1.0f;  // new, or put back on the table: there at once
        m.from = jump ? to : at;
        memcpy(m.qFrom, jump ? q : shown, sizeof m.qFrom);
        m.to = to;
        memcpy(m.qTo, q, sizeof m.qTo);
        m.ms = now;
        if (jump) Place(o, to, q);
        if (o->IsVisible() != visible) Cmd(0x0750).I(m.handle).I(visible ? 1 : 0).Run();  // SET_OBJECT_VISIBLE
    }
    for (auto it = g_join.mirrors.begin(); it != g_join.mirrors.end();)
        if (!seen.count(it->first)) {
            DeleteMirror(it->second);
            it = g_join.mirrors.erase(it);
        } else {
            ++it;
        }
    // 28.10: no balls at all for the first seconds of a game for two, with nothing to tell why
    int shown = 0, hidden = 0;
    for (auto& [slot, m] : g_join.mirrors)
        if (CObject* o = m.handle >= 0 ? GetObject(m.handle) : nullptr) ++(o->IsVisible() ? shown : hidden);
    if ((shown << 8 | hidden) != g_join.told) {
        g_join.told = shown << 8 | hidden;
        LOGI("Pool for two: %d of the table's %d objects made here, %d shown", shown + hidden, (int)n, shown);
    }
    // the balls were made and shown here, and the second player saw none (28-29.10): one of them as it stands, once
    for (auto& [slot, m] : g_join.mirrors) {
        CObject* o = m.handle >= 0 && !g_join.ballTold && slot != g_join.cueSlot ? GetObject(m.handle) : nullptr;
        if (!o || !o->IsVisible()) continue;
        g_join.ballTold = true;
        CVector at = o->GetPosition(), table{0, 0, 0};
        if (CPoolHeader* pool = ObjectPool())  // the table here (k_pooltablesm, k_pooltableb), to compare the heights
            for (int i = 0; i < pool->size; ++i) {
                auto* t = (CObject*)(pool->objects + (size_t)i * kObjectPoolElemSize);
                if (pool->byteMap[i] >= 0 && TableModel(t->ModelIndex()) && (t->GetPosition() - at).Length() < 5.0f)
                    table = t->GetPosition();
            }
        LOGI("Pool for two: object %d (model %d) at %.2f,%.2f,%.2f, area %d (ours %d), model in place %d, on screen %d, "
             "%.1f m from the camera, the table here at %.2f,%.2f,%.2f", (int)slot, (int)o->ModelIndex(), at.x, at.y, at.z,
             (int)o->AreaCode(), Ref<int32_t>(0xB72914), o->RwObject() != nullptr, (int)CallMethod<0x534540, bool>(o),
             (at - cam::Source()).Length(), table.x, table.y, table.z);
        break;
    }
}

// our player in the opponent's place, playing its animation at its time
void Puppet() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!g_join.placed || !me || me->InVehicle()) return;
    me->CurrentRotation() = me->AimingRotation() = g_join.heading;
    // frozen, the player's matrix kept its old way while the angle turned: it struck across the table (28.10, "Карл не в
    // ту сторону бьет")
    CallMethod<0x43E0C0>(me, g_join.heading);  // CPlaceable::SetHeading
    SetEntityPosition(me, g_join.at);
    me->MoveSpeed() = CVector{};
    void* clump = me->RwObject();
    void* h = g_join.block >= 0 ? anim::Resolve(g_join.block, g_join.idx) : nullptr;
    if (!clump || !h) return;
    void* mine = nullptr;
    for (void* a = anim::First(clump); a; a = anim::Next(a))
        if (anim::Hierarchy(a) == h && anim::BlendDelta(a) >= 0.0f) mine = a;
    if (!mine) mine = anim::Blend(clump, h, g_join.flags, 8.0f);
    if (!mine) return;
    Field<float>(mine, 0x24) = g_join.speed;  // CAnimBlendAssociation::m_fSpeed
    float t = g_join.time + (float)(NowMs() - g_join.viewMs) / 1000.0f * g_join.speed, total = anim::TotalTime(h);
    t = (g_join.flags & anim::F_LOOPED) && total > 0.0f ? std::fmod(t, total) : std::min(t, total);
    if (std::fabs(anim::CurrentTime(mine) - t) > 0.15f) anim::SetCurrentTime(mine, t);
    g_join.applied = h;
}

// The opponent's cue in our player's hand, as the owner's game gives it to our copy there (HandCue), with its own
// animation: POOL2 sets that one's time from its holder's each frame, so here it goes on at our player's pace from the view.
// Not one of our props (Props): the owner's game has the cue itself.
void HoldCue() {
    CPlayerPed* me = FindPlayerPed(0);
    int cue = CueMirror();
    CObject* o = cue >= 0 ? GetObject(cue) : nullptr;
    if (!g_join.placed || !me || me->InVehicle() || !o) return;
    int ref = GetPedRef(me);
    // IS_CHAR_HOLDING_OBJECT; TASK_PICK_UP_OBJECT as POOL2 gives it: into the right hand, no animation
    if (!Cmd(0x0737).I(ref).I(cue).Run()) Cmd(0x070A).I(ref).I(cue).F(0.0f).F(0.0f).F(0.0f).I(6).I(16).S("NULL").S("NULL").I(1).Run();
    auto* clump = (uint8_t*)o->RwObject();
    if (!clump || clump[0] != 2) return;  // not built yet, or an atomic: nothing in it moves
    void* h = g_join.cueBlock >= 0 ? anim::Resolve(g_join.cueBlock, g_join.cueIdx) : nullptr;
    if (!h && g_join.cueBlock >= 0) return;  // its IFP is being loaded
    if (!Call<0x4D6760, bool>(clump)) Call<0x4D6720>(clump);  // RpAnimBlendClumpIsInitialized, RpAnimBlendClumpInit
    void* mine = nullptr;
    for (void* a = anim::First(clump); a; a = anim::Next(a))
        if (anim::Hierarchy(a) == g_join.cueApplied && anim::BlendDelta(a) >= 0.0f) mine = a;
    if (mine && h != g_join.cueApplied) {  // another one there now: Blend takes the old one out, none there does it here
        if (!h) anim::FadeOut(mine);
        mine = nullptr;
    }
    if (h && !mine) mine = anim::Blend(clump, h, g_join.cueFlags, 1000.0f);  // at once, as PLAY_OBJECT_ANIM's
    g_join.cueApplied = h;
    if (!mine) return;
    Field<float>(mine, 0x24) = 0.0f;  // CAnimBlendAssociation::m_fSpeed: its time is set here
    float t = g_join.cueTime + (float)(NowMs() - g_join.viewMs) / 1000.0f * g_join.speed, total = anim::TotalTime(h);
    anim::SetCurrentTime(mine, (g_join.cueFlags & anim::F_LOOPED) && total > 0.0f ? std::fmod(t, total) : std::clamp(t, 0.0f, total));
}

// our own bar's opponent waits where the owner's stood at the start: two people in one place
void HideOwnOpponent() {
    if (!g_join.placed || NowMs() - g_join.hideMs < 500) return;
    g_join.hideMs = NowMs();
    auto hide = [](CPed* ped) {
        ped->Flags() &= ~0x80u;
        g_join.hidden.push_back(ped);
    };
    CPoolHeader* pool = PedPool();
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* ped = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        if (!ped->IsPlayer() && ped->IsVisible() && (ped->GetPosition() - g_join.start).Length() <= 2.0f) hide(ped);
    }
    // the one the table's attractor (POOL) keeps in its 1@ waits by the table farther off: it stood in the owner's camera
    // in the opponent's place, idle (28.10, the second player's pictures: BMYPOL2 in a green jacket)
    for (void* s = Ref<void*>(0xA8B42C); s; s = Field<void*>(s, 0x00))  // CTheScripts::pActiveScripts
        if (!_strnicmp(Field<char[8]>(s, 0x08), "pool", 8))
            if (CPed* own = GetPed(Field<int32_t>(s, 0x3C + 1 * 4)); own && !own->IsPlayer() && own->IsVisible())  // m_aLocalVars[1]
                hide(own);
}

void ApplyCamera() {
    uint64_t now = NowMs();
    float frame = std::min(0.1f, (now - g_join.shownMs) / 1000.0f);
    g_join.shownMs = now;
    if ((g_join.src - g_join.shownSrc).Length() > 3.0f) {  // a cut
        g_join.shownSrc = g_join.src, g_join.shownFront = g_join.front, g_join.shownFov = g_join.fov;
    } else {
        float k = 1.0f - std::exp(-12.0f * frame);
        g_join.shownSrc += (g_join.src - g_join.shownSrc) * k;
        g_join.shownFront += (g_join.front - g_join.shownFront) * k;
        g_join.shownFov += (g_join.fov - g_join.shownFov) * k;
    }
    g_join.shownFront = g_join.shownFront.Normalized();
    Ref<CVector>(addr::TheCamera + 0xCE0) = g_join.shownSrc;                              // m_vecMoveLinear
    Ref<CVector>(addr::TheCamera + 0xC80) = g_join.shownSrc + g_join.shownFront * 10.0f;  // m_vecTrackLinear
    Ref<float>(addr::TheCamera + 0xCB8) = std::clamp(g_join.shownFov, 5.0f, 170.0f);      // m_fFOVNew
}

// our buttons and mouse to the owner's game, every frame: they are its second pad
void SendInput() {
    Writer w(proto::G_POOL);
    w.U8(OP_INPUT), w.U8(g_join.from);
    int16_t v[24];
    memcpy(v, &GetPad(0)->NewState, sizeof v);
    for (int i = 0; i < 20; ++i) w.I16(v[i]);
    w.U8(Cmd(0x0A4B).Run() ? 1 : 0);  // IS_PC_USING_JOYPAD: how we play
    w.F32(Ref<float>(kMouse + 0x0C)), w.F32(Ref<float>(kMouse + 0x10));
    Net().Send(proto::CH_STATE, w);
}

void JoinUpdate() {
    if (g_join.from == 0xFF) return;
    if (NowMs() - g_join.viewMs > kQuietMs) {  // the owner's game went quiet (its table gone with it)
        LOGI("Pool for two: no view of the table for %llu s", (unsigned long long)(kQuietMs / 1000));
        return JoinEnd();
    }
    Puppet();
    HoldCue();
    MoveMirrors();
    HideOwnOpponent();
    SendInput();
}

// ---------------------------------------------------------------- the table: POOL2 lowers it
// POOL2 lowers its table by 10 cm for a game and lays the balls on it there (SET_OBJECT_COORDINATES of its 163@, raised
// again at its end). The other game's table stood where the map has it, and the owner's balls there (a game for two's
// copies, or a game alone's, Props) lay inside it: the second player saw none (28-30.10). A game with a POOL2 of its own
// tells everyone its table's pose; the others move their own table there, and back once that stops.
struct Table {
    int ref = -1;     // our own table, standing as the other game's
    CVector pos;      // where it stood here before
    float q[4] = {0, 0, 0, 1};
    uint64_t ms = 0;  // the other game's last word of it
};
std::map<uint8_t, Table> g_tables;  // by the player whose game moved its table
CVector g_toldTable;                // our POOL2's table where it was told last, and when
uint64_t g_toldTableMs = 0;

void SendTable() {
    CObject* t = Pool2() ? GetObject(Local(kTable)) : nullptr;
    if (!t || !TableModel(t->ModelIndex()) || !t->Matrix()) return;
    CVector at = t->GetPosition();
    if ((at - g_toldTable).Length() < 0.01f && NowMs() - g_toldTableMs < kTableMs) return;
    g_toldTable = at, g_toldTableMs = NowMs();
    Quat r = Quat::FromMatrix(*t->Matrix());
    Writer w(proto::G_POOL);
    w.U8(OP_TABLE), w.U8(0xFF);  // everyone's
    w.U16((uint16_t)t->ModelIndex()), w.Vec({at.x, at.y, at.z});
    w.F32(r.x), w.F32(r.y), w.F32(r.z), w.F32(r.w);
    Net().Send(proto::CH_STATE, w);
}

void PutTableBack(Table& t) {
    if (CObject* o = t.ref >= 0 ? GetObject(t.ref) : nullptr) {
        Place(o, t.pos, t.q);
        LOGI("Pool table at %.2f,%.2f put back here", t.pos.x, t.pos.y);
    }
    t.ref = -1;
}

void ReadTable(uint8_t from, Reader& r) {
    uint16_t model = r.U16();
    Vec3 p = r.Vec();
    float q[4];
    for (float& f : q) f = r.F32();
    if (!r.Ok() || !TableModel(model) || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
        !std::all_of(q, q + 4, [](float f) { return std::isfinite(f); }))
        return;
    if (Pool2()) return;  // our own game has our table
    CVector at{p.x, p.y, p.z};
    Table& t = g_tables[from];
    CObject* o = t.ref >= 0 ? GetObject(t.ref) : nullptr;
    if (!o || o->ModelIndex() != model) {
        // the same table here: of its model, within a metre (the other game moved it by centimetres)
        o = nullptr;
        float best = 1.0f;
        if (CPoolHeader* pool = ObjectPool())
            for (int i = 0; i < pool->size; ++i) {
                auto* c = (CObject*)(pool->objects + (size_t)i * kObjectPoolElemSize);
                float d = pool->byteMap[i] >= 0 && c->ModelIndex() == model && c->Matrix() ? (c->GetPosition() - at).Length() : best;
                if (d < best) best = d, o = c;
            }
        if (!o) return;
        t.ref = GetObjectRef(o);
        t.pos = o->GetPosition();
        Quat was = Quat::FromMatrix(*o->Matrix());
        t.q[0] = was.x, t.q[1] = was.y, t.q[2] = was.z, t.q[3] = was.w;
    }
    t.ms = NowMs();
    if ((o->GetPosition() - at).Length() <= 0.005f) return;
    LOGI("Pool table at %.2f,%.2f: height %.2f here, as in %s's game (%.2f here before)", at.x, at.y, at.z, Name(from).c_str(),
         o->GetPosition().z);
    Place(o, at, q);
}

// The second player let go as POOL2 lets its own two go at its end: on the floor at its waiting place (225@, the table's
// 0,-1.8), and not before that floor is there. Let go where the opponent last stood, the partner fell through the bar's
// floor right after the game (29-30.10, twice).
struct Leave {
    CVector at;
    uint64_t untilMs = 0;
} g_leave;

void LetGo(CPlayerPed* me) {
    CObject* table = nullptr;
    if (auto it = g_tables.find(g_join.from); it != g_tables.end() && it->second.ref >= 0) table = GetObject(it->second.ref);
    if (CPoolHeader* pool = ObjectPool(); !table && pool)  // an owner's game that tells no table: the one by us
        for (int i = 0; i < pool->size; ++i) {
            auto* t = (CObject*)(pool->objects + (size_t)i * kObjectPoolElemSize);
            if (pool->byteMap[i] >= 0 && TableModel(t->ModelIndex()) && (t->GetPosition() - me->GetPosition()).Length() < 3.0f) table = t;
        }
    g_leave.at = table && table->Matrix() ? table->GetPosition() - table->Matrix()->forward * 1.8f : me->GetPosition();
    g_leave.untilMs = NowMs() + 5000;
    // the interior's collision comes with our player's own area (CColStore::LoadCollision): the visible one's
    if (int area = Ref<int32_t>(0xB72914); area && me->AreaCode() != area) {
        LOGI("Pool for two: our player's area was %d, the visible one %d", (int)me->AreaCode(), area);
        Cmd(0x0860).I(GetPedRef(me)).I(area).Run();  // SET_CHAR_AREA_VISIBLE
    }
}

void LeaveUpdate() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || g_join.from != 0xFF) {  // no player, or at a table for two again (frozen by it)
        g_leave = {};
        return;
    }
    bool found = false;
    float z = Call<0x5696C0, float>(g_leave.at.x, g_leave.at.y, g_leave.at.z + 1.0f, &found, (CEntity**)nullptr);  // CWorld::FindGroundZFor3DCoord
    bool floor = found && z > g_leave.at.z - 2.5f;  // not the ground under the interior
    if (!floor && NowMs() < g_leave.untilMs) return;
    int h = GetPedRef(me);
    if (floor) Cmd(0x00A1).I(h).F(g_leave.at.x).F(g_leave.at.y).F(z).Run();  // SET_CHAR_COORDINATES: on it
    Cmd(0x04D7).I(h).I(0).Run();  // FREEZE_CHAR_POSITION off
    LOGI("Pool for two: let go at %.1f,%.1f,%.1f%s", g_leave.at.x, g_leave.at.y, floor ? z : g_leave.at.z, floor ? "" : ", no floor found there");
    g_leave = {};
}

void TablesUpdate() {
    SendTable();
    if (Pool2()) return;  // our own game has our table: the other game's waits
    for (auto it = g_tables.begin(); it != g_tables.end();)
        if (NowMs() - it->second.ms > kTableQuietMs) {
            PutTableBack(it->second);
            it = g_tables.erase(it);
        } else {
            ++it;
        }
}

void Handle(uint8_t from, Reader& r) {
    uint8_t op = r.U8(), to = r.U8();
    if (op == OP_TABLE) {  // to everyone
        if (r.Ok() && from != Net().MyId() && core::IsPlayerReady()) ReadTable(from, r);
        return;
    }
    if (!r.Ok() || to != Net().MyId() || from == Net().MyId() || !core::IsPlayerReady()) return;
    switch (op) {
        case OP_START:
            JoinStart(from);
            break;
        case OP_END:
            if (from == g_join.from) JoinEnd();
            break;
        case OP_FAIL: {
            bool away = r.U8() != 0;
            ui::TheChat().AddSystem(Format(away ? TR("Игрок %s сейчас не за столом: сыграйте свою партию на деньги, сравнится выигрыш.",
                                                     "%s is not at the table now: play your own game for money, the winnings are compared.")
                                                : TR("У игрока %s уже идёт партия с соперником из бара: сыграйте свою за другим столом.",
                                                     "%s plays the bar's player already: play your own game at another table."),
                                           Name(from).c_str()));
            break;
        }
        case OP_RESULT: {
            bool second = r.U8() != 0;
            if (!r.Ok() || from != g_join.from) return;
            ui::TheChat().AddSystem(second ? TR("Партия: побеждаете вы.", "The game: you win.")
                                           : Format(TR("Партия: побеждает %s.", "The game: %s wins."), Name(from).c_str()));
            break;
        }
        case OP_VIEW:
            if (from == g_join.from) ReadView(r);
            break;
        case OP_OBJECTS:
            if (from == g_join.from) ReadObjects(r);
            break;
        case OP_INPUT:
            if (from == g_own.with && g_own.on && !g_own.alone) ReadInput(r);
            break;
    }
}

class PoolModule : public Module {
public:
    const char* Name() const override { return "Pool"; }
    void OnInstallHooks() override { Net().SetHandler(proto::G_POOL, &Handle); }
    void OnPreProcess() override {
        if (g_join.camOn) ApplyCamera();  // the camera's processing takes the three values at its end
    }
    void OnPostProcess() override {
        if (g_leave.untilMs && core::IsPlayerReady()) LeaveUpdate();  // after a session's end too
        if (!Net().Online() || !core::IsPlayerReady()) return;
        OwnerUpdate();
        JoinUpdate();
        TablesUpdate();
    }
    void OnPeerLeave(uint8_t id) override {
        if (id == g_own.with && g_own.on && !g_own.alone) OwnerAlone();
        else if (id == g_own.with && !g_own.on) g_own = {};
        if (id == g_join.from) JoinEnd();
        if (auto it = g_tables.find(id); it != g_tables.end()) it->second.ms = 0;  // put back by TablesUpdate
    }
    void OnSessionEnd() override {
        if (g_own.on && !g_own.alone) OwnerAlone();
        else if (!g_own.on) g_own = {};
        JoinEnd();
        if (!Pool2())
            for (auto& [id, t] : g_tables) PutTableBack(t);
        g_tables.clear();
    }
    void OnGameShutdown() override {  // the world goes: what was ours in it too
        g_own = {};
        g_join = {};
        g_tables.clear();
        g_leave = {};
    }
};

PoolModule g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

bool Ask(uint8_t partner) {
    if (!Pool2() || Local(kTwoPlayers) == 1 || g_own.with != 0xFF) {
        Send(OP_FAIL, partner, Pool2() ? 0 : 1);  // it was told a game for two starts now
        return false;
    }
    g_own = {};
    g_own.with = partner;
    g_own.askUntil = NowMs() + kAskMs;
    LOGI("Pool for two asked of our table (%s)", Name(partner).c_str());
    return true;
}

bool Owning() { return g_own.with != 0xFF && g_own.on && !g_own.alone; }

bool TextsFrom(uint8_t id) { return g_join.from != 0xFF && id == g_join.from; }

int Answer(void* script, uint16_t op) {
    // LOCATE_CHAR_ON_FOOT_CHAR_3D of the table's attractor (POOL, no mission): the second player stands at the owner's
    // table by its own bar's opponent, whose F started a game of its own over the owner's: a second POOL2 with its own
    // balls, camera and animations (28.10)
    if (op == 0x0105 && g_join.from != 0xFF && !_strnicmp(Field<char[8]>(script, 0x08), "pool", 8)) return 0;
    if (g_own.with == 0xFF || !Field<uint8_t>(script, 0xDC) || script != Pool2()) return -1;
    const auto* p = (const int32_t*)kScriptParams;
    // IS_CHAR_HOLDING_OBJECT of the opponent and its cue: our copy of the second player holds it (HandCue)
    if (op == 0x0737 && g_own.cue >= 0 && p[0] == g_own.opponent && p[1] == g_own.cue) return 1;
    if (op == 0x00E1 && p[0] == 1) {  // IS_BUTTON_PRESSED on the second pad
        if (!g_own.on) return g_own.askUntil && p[1] == 12 ? 1 : -1;  // START at the bet screen: the second player joins
        const CControllerState& pad = g_own.alone ? GetPad(0)->NewState : g_own.pad;
        return Local(kTurn) == 1 && Button(pad, p[1]) != 0 ? 1 : 0;
    }
    if (op == 0x0A4B && g_own.on && !g_own.alone && Local(kTurn) == 1) return g_own.joypad ? 1 : 0;  // IS_PC_USING_JOYPAD
    return -1;
}

void AfterCommand(void* script, uint16_t op, const uint8_t* ip) {
    if (g_own.with == 0xFF || !g_own.on || script != Pool2()) return;
    bool theirs = Local(kTurn) == 1;
    int32_t padNo = -1;  // in the code: ScriptParams hold the sticks by now
    if (op == 0x0494 && game::script::ArgValue(ip + 2, padNo) && padNo == 1 && theirs) {  // GET_POSITION_OF_ANALOGUE_STICKS of the second pad
        const CControllerState& pad = g_own.alone ? GetPad(0)->NewState : g_own.pad;
        int16_t v[4] = {pad.LeftStickX, pad.LeftStickY, pad.RightStickX, pad.RightStickY};
        const uint8_t* a = NextArg(ip + 2);  // past the pad
        for (int i = 0; i < 4 && a; ++i, a = NextArg(a))
            if (int32_t* out = OutVar(a)) *out = v[i];
    } else if (op == 0x0A4A && theirs && !g_own.alone) {  // GET_PC_MOUSE_MOVEMENT: the second player's mouse
        float m[2] = {g_own.mx, g_own.my};
        const uint8_t* a = ip + 2;
        for (int i = 0; i < 2 && a; ++i, a = NextArg(a))
            if (int32_t* out = OutVar(a)) memcpy(out, &m[i], 4);
        g_own.mx = g_own.my = 0.0f;
    } else if (op == 0x00BA && ip[2] == 0x09 && !g_own.alone &&
               (!memcmp(ip + 3, "PL_08", 6) || !memcmp(ip + 3, "PL_09", 6))) {  // PRINT_BIG: the first player won, or the second
        Won(ip[7] == '9' ? 1 : 0);
    }
}

}  // namespace coop::pool

namespace coop {

bool PoolTableCopy(void* object) {
    for (auto& [slot, m] : pool::g_join.mirrors)
        if (m.handle >= 0 && (void*)game::GetObject(m.handle) == object) return true;
    return false;
}

}  // namespace coop
