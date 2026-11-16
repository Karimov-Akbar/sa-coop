// The player's skin (Skins.h), a players' wish (09.11): chosen in a window, seen by the others, kept in the cutscenes.
//  * Put on as SET_PLAYER_MODEL puts one (Players::SetModel, the copies' way): a new clump of that model, the walk set
//    kept. CJ again is model 0 with his clothes built anew (CClothes::RebuildPlayer). A shop, a haircut or a mission's
//    dress rebuild no ped's model in CJ's clothes meanwhile (Patches, ConstructPedModel).
//  * Kept in coop.ini ([player] skin: a model number, or an own skin's file, name.dff) and put on again once a game
//    started and CJ is free (a load makes him CJ: CPools::LoadPedPool makes the player of model 0 whatever was saved).
//  * The player's own skins (coop\skins) are loaded when shown or worn (entities/skin) and taken out of memory again
//    when the window closes, all but the worn one and those a copy or a cutscene uses.
//  * A cutscene's CJ (CSPLAY, model 1) is made of the story's CJ's worn model instead (CCutsceneMgr::CreateCutsceneObject):
//    our player's in our own cutscenes, the host's in the host's on a partner's screen. The scene's animation moves its
//    body once its tracks are bound to the skin's bones by their numbers (HookSetCutsceneAnim: the game binds them to
//    CSPLAY's 61 bones by their places). Its face has none of the bones the scene's face moves (CJ's 5001-5026 against a
//    ped's jaw and brows, 6-8), and a bone no animation moves gets no rotation at all (the jaw's rest is 111 degrees
//    from none: a crooked mouth): the jaw and the brows keep the standing's pose (WriteFace), and the jaw talks with the
//    ped's FacTalk while the scene's CJ moves his (jaw1, 5015). Its shoulders are turned as CSPLAY's are
//    (CCutsceneObject::PreRender, CPed::ShoulderBoneRotation) only when it is rigged as CJ (ShoulderHelpers): anyone
//    else's 301 and 302 lie on the chest and keep the standing's pose too. The object is remembered from the cutscene's
//    loading till its data is deleted (CutsceneLoaded): none of this ran before 11.11, see OnPostProcess.
//  * Its voice: such a cutscene plays its own track with another voice in place of CJ's (entities/skin, SceneVoice).
#include "features/skins/Skins.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>

#include "common/lib/ini/Ini.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/skin/OwnSkins.h"
#include "entities/skin/SceneVoice.h"
#include "shared/config/Config.h"
#include "shared/game/Anim.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop::skins {

namespace {
using namespace game;

constexpr int kFirst = 7, kLast = 288;  // the ordinary people: before them CJ and the cutscene's, after them the story's
constexpr uintptr_t kClumpOffset = 0xB5F878;  // RpAnimBlendClumpGetData: the plugin's place in a clump

std::vector<Skin> g_list;
size_t g_standard = 0;  // the game's in g_list, the own skins after them

bool CutsceneOn() { return Ref<uint8_t>(0xB5F851) != 0; }  // CCutsceneMgr::ms_running
// A cutscene's objects are made while it loads (LoadCutsceneData_loading) and deleted with its data: ms_running comes
// up frames later, once the script starts it
bool CutsceneLoaded() { return Ref<int32_t>(0xB5F84C) != 0; }  // CCutsceneMgr::ms_cutsceneLoadStatus

Group GroupOf(int model) {
    void* info = ModelInfoOf(model);
    int type = info ? Field<int32_t>(info, 0x28) : 4;  // CPedModelInfo::m_nPedType
    if (type == 5 || type == 22) return WOMEN;                 // CIVFEMALE, PROSTITUTE
    if (type >= 7 && type <= 16) return GANGS;                 // GANG1-10
    if (type == 6 || type == 18 || type == 19) return SERVICE;  // COP (SWAT, army), MEDIC, FIREMAN
    return MEN;
}

// The names the game's files give them (peds.ide): the model infos keep only their keys
std::string NameOf(int model) {
    static std::vector<std::string> names;
    if (names.empty()) {
        names.resize(kLast + 1);
        std::vector<uint8_t> bytes;
        if (ReadFileBytes(Cfg().gameDir + L"data\\peds.ide", bytes)) {
            std::string text(bytes.begin(), bytes.end());
            bool peds = false;
            for (const std::string& raw : Split(text, '\n')) {
                std::string line = Trim(raw);
                std::string low = ToLower(line);
                if (low == "peds") peds = true;
                else if (low == "end") peds = false;
                else if (peds && !line.empty() && line[0] != '#') {
                    auto parts = Split(line, ',');
                    int id = parts.size() > 1 ? atoi(parts[0].c_str()) : -1;
                    if (id >= 0 && id <= kLast) names[id] = Trim(parts[1]);
                }
            }
        }
    }
    return model >= 0 && model <= kLast && !names[model].empty() ? names[model] : std::to_string(model);
}

// coop.ini [player] skin: a model number of the game's, or an own skin's file (name.dff)
std::string OwnChosen() {
    const std::string& v = Cfg().skin;
    return v.size() > 4 && ToLower(v.substr(v.size() - 4)) == ".dff" ? v.substr(0, v.size() - 4) : std::string();
}
int StandardChosen() {
    int m = atoi(Cfg().skin.c_str());
    return OwnChosen().empty() && m >= kFirst && m <= kLast ? m : 0;
}
// the model to wear by choice, an own skin loaded now; 0: CJ, -1: it would not load
int ChosenModel() {
    std::string own = OwnChosen();
    return own.empty() ? StandardChosen() : skin::OwnModel(own);
}

// The mod's skin packs (coop\skinpacks\<folder>), in the window's order under their titles; a folder not named here
// comes after them under its own name. The authors allowed the packs in the mod (10.11; Criminal Peds Vary's gangs 12.11).
const char* const kPacks[] = {"families", "ballas", "vagos", "aztecas", "rifa", "danang", "triads",
                              "italians", "russians", "angels", "beta", "unused"};

std::string PackTitle(int place, const std::string& folder) {
    std::string cpv = TR(" · wizvich и другие", " · wizvich and others");
    switch (place) {
        case 0: return "Families · CemeteryBlunts";
        case 1: return "Ballas · CemeteryBlunts";
        case 2: return "Vagos · CemeteryBlunts";
        case 3: return "Aztecas · CemeteryBlunts";
        case 4: return "San Fierro Rifa" + cpv;
        case 5: return "Da Nang Boys" + cpv;
        case 6: return TR("Триады", "Triads") + cpv;
        case 7: return TR("Итальянская мафия", "Italian Mafia") + cpv;
        case 8: return TR("Русская мафия", "Russian Mafia") + cpv;
        case 9: return "Angels of Death MC" + cpv;
        case 10: return TR("Бета-версии прохожих · Camel, Shiro_Mori и другие", "Beta pedestrians · Camel, Shiro_Mori and others");
        case 11: return std::string(TR("Вырезанные из игры", "Cut from the game")) + " · Jéssica Natália, Junior_Djjr, falsenail";
        default: return folder;
    }
}

// names with their numbers in order: FAM4 before FAM10
bool NameBefore(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isdigit((unsigned char)a[i]) && isdigit((unsigned char)b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && isdigit((unsigned char)a[i2])) ++i2;
            while (j2 < b.size() && isdigit((unsigned char)b[j2])) ++j2;
            long x = atol(a.substr(i, i2 - i).c_str()), y = atol(b.substr(j, j2 - j).c_str());
            if (x != y) return x < y;
            i = i2, j = j2;
        } else {
            if (a[i] != b[j]) return a[i] < b[j];
            ++i, ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

void AddPacked() {
    std::vector<std::pair<int, Skin>> rows;  // by the pack's place
    for (const std::string& key : skin::PackSkins()) {
        std::string folder = key.substr(0, key.find('/')), shown = key.substr(key.find('/') + 1);
        int place = (int)std::size(kPacks);
        for (int i = 0; i < (int)std::size(kPacks); ++i)
            if (folder == kPacks[i]) place = i;
        for (char& c : shown) c = (char)toupper((unsigned char)c);  // as the game's are shown
        rows.push_back({place, {0, PACKS, shown, key, PackTitle(place, folder)}});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return a.second.pack != b.second.pack ? a.second.pack < b.second.pack : NameBefore(a.second.name, b.second.name);
    });
    for (auto& row : rows) g_list.push_back(std::move(row.second));
}

void Keep(const std::string& value) {
    Cfg().skin = value;
    Ini ini;
    ini.Load(Cfg().iniPath);
    ini.Set("player", "skin", value);
    ini.Save(Cfg().iniPath);
}

// ---- the cutscene's CJ made of the skin

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

const uint8_t* SequenceOf(void* hier, uint32_t bone) {  // CAnimBlendHierarchy's sequence of that bone (by its tag)
    auto* seqs = hier ? Field<const uint8_t*>(hier, 0x04) : nullptr;
    int n = hier ? Field<uint16_t>(hier, 0x08) : 0;
    for (int i = 0; seqs && i < n; ++i) {
        const uint8_t* s = seqs + i * 0x0C;
        if ((Field<uint16_t>(s, 0x04) & 0x10) && Field<uint16_t>(s, 0x00) == bone) return s;  // m_IsUsingBoneTag, m_BoneTag
    }
    return nullptr;
}

// a sequence's rotation at t (s): its key frames hold the time from the one before (compressed: 1/4096 and 1/60 s)
bool RotationAt(const uint8_t* seq, float t, Quat& out) {
    uint16_t flags = seq ? Field<uint16_t>(seq, 0x04) : 0;
    int frames = seq ? Field<uint16_t>(seq, 0x06) : 0;
    auto* f = seq ? Field<const uint8_t*>(seq, 0x08) : nullptr;
    if (!f || !frames || !(flags & 1)) return false;
    bool packed = flags & 4, moves = flags & 2;
    size_t size = packed ? (moves ? 16 : 10) : (moves ? 32 : 20);
    auto key = [&](int i, Quat& q, float& dt) {
        const uint8_t* k = f + i * size;
        if (packed) {
            auto* s = (const int16_t*)k;
            q = {s[0] / 4096.0f, s[1] / 4096.0f, s[2] / 4096.0f, s[3] / 4096.0f}, dt = s[4] / 60.0f;
        } else {
            auto* v = (const float*)k;
            q = {v[0], v[1], v[2], v[3]}, dt = v[4];
        }
    };
    Quat prev;
    float at = 0.0f, dt = 0.0f;
    key(0, prev, at);
    out = prev;
    for (int i = 1; i < frames && t > at; ++i) {
        Quat q;
        key(i, q, dt);
        if (t <= at + dt && dt > 0.0f) {  // between the two, the short way round
            float u = (t - at) / dt, sign = prev.x * q.x + prev.y * q.y + prev.z * q.z + prev.w * q.w < 0.0f ? -1.0f : 1.0f;
            Quat r{prev.x + (q.x * sign - prev.x) * u, prev.y + (q.y * sign - prev.y) * u, prev.z + (q.z * sign - prev.z) * u,
                   prev.w + (q.w * sign - prev.w) * u};
            float len = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
            if (len > 0.0f) r = {r.x / len, r.y / len, r.z / len, r.w / len};
            out = r;
            return true;
        }
        at += dt, prev = q, out = q;
    }
    return true;
}

void* g_csCj = nullptr;  // the cutscene's CJ made of the skin
bool g_csShoulders = false;  // it has the bones CPed::ShoulderBoneRotation turns
float g_talkTime = 0.0f;
uint64_t g_talkUntil = 0, g_faceMs = 0;

void* PedAnimation(const char* name) { return Call<0x4D42F0, void*>(name, (void*)anim::Block(0)); }  // CAnimManager::GetAnimation, "ped"

// The bones the scene's animation does not move, written before the hierarchy's matrices are made of the key frames:
// the face's (6 L Brow, 7 R Brow, 8 Jaw), and the chest's (301, 302) of a skin not rigged as CJ, whose tracks are left
// out (HookSetCutsceneAnim). The standing's pose is the one these bones are bound in: the jaw's exactly, the chest's
// within 8 degrees, the brows' within 16 as in the game's own play (checked on the packs' and the game's models)
void WriteFace(void* o) {
    auto* clump = Field<uint8_t*>(o, 0x18);  // CEntity::m_pRwObject
    auto* data = clump && clump[0] == 2 ? Field<uint8_t*>(clump, Ref<uint32_t>(kClumpOffset)) : nullptr;
    void* rest = PedAnimation("IDLE_stance");
    void* talk = PedAnimation("factalk");
    auto* frames = data ? Field<uint8_t*>(data, 0x10) : nullptr;  // CAnimBlendClumpData::m_FrameDatas
    int n = data ? Field<int32_t>(data, 0x08) : 0;
    for (int i = 0; frames && i < n; ++i) {
        uint8_t* fd = frames + i * 0x18;  // AnimBlendFrameData
        uint32_t bone = Field<uint32_t>(fd, 0x14);
        Quat q;
        bool talking = bone == 8 && talk && NowMs() < g_talkUntil;
        bool ours = (bone >= 6 && bone <= 8) || (!g_csShoulders && (bone == 301 || bone == 302));
        if (!ours || !RotationAt(SequenceOf(talking ? talk : rest, bone), talking ? g_talkTime : 0.0f, q)) continue;
        Field<uint8_t>(fd, 0x00) |= 0x02;  // KeyFramesIgnoreNodeOrientation: the animation update leaves it to us
        if (auto* key = Field<float*>(fd, 0x10)) key[0] = q.x, key[1] = q.y, key[2] = q.z, key[3] = q.w;  // its KeyFrame's rotation
    }
}

// the scene's CJ talks while his jaw moves (jaw1, 5015): 1.7 degrees in 0.15 s, held a quarter of a second
void UpdateTalk() {
    float dt = std::clamp((float)(NowMs() - g_faceMs) / 1000.0f, 0.0f, 0.1f);
    g_faceMs = NowMs();
    auto* clump = (uint8_t*)Field<void*>(g_csCj, 0x18);
    void* a = clump && clump[0] == 2 ? anim::First(clump) : nullptr;
    const uint8_t* jaw = a ? SequenceOf(anim::Hierarchy(a), 5015) : nullptr;
    Quat now, before;
    float t = a ? anim::CurrentTime(a) : 0.0f;
    if (jaw && RotationAt(jaw, t, now) && RotationAt(jaw, std::max(0.0f, t - 0.15f), before)) {
        float dot = std::fabs(now.x * before.x + now.y * before.y + now.z * before.z + now.w * before.w);
        if (2.0f * std::acos(std::min(1.0f, dot)) > 0.03f) g_talkUntil = NowMs() + 250;
    }
    void* talk = PedAnimation("factalk");
    float total = talk ? anim::TotalTime(talk) : 0.0f;
    g_talkTime = NowMs() < g_talkUntil && total > 0.0f ? std::fmod(g_talkTime + dt, total) : 0.0f;
}

using CreateCsObjectFn = void*(__cdecl*)(int);
CreateCsObjectFn g_origCreateCsObject = nullptr;
void* __cdecl HookCreateCutsceneObject(int model) {
    // MODEL_CSPLAY plays the story's CJ, made of the skin he wears (skin::SceneCjModel: the scene's voice goes by it too)
    int skin = model == 1 ? skin::SceneCjModel() : 0;
    bool swap = skin != 0;
    void* o = g_origCreateCsObject(swap ? skin : model);
    if (model == 1) {
        g_csCj = swap ? o : nullptr;
        g_talkUntil = 0, g_talkTime = 0.0f, g_faceMs = NowMs();
        // the shoulders turned as CSPLAY's only on a skin rigged as CJ (ShoulderHelpers); without the bones the game's
        // turn writes before the bones' matrices (RpHAnimIDGetIndex gives -1)
        auto* clump = g_csCj ? Field<uint8_t*>(o, 0x18) : nullptr;
        g_csShoulders = clump && clump[0] == 2 && ShoulderHelpers(clump);
        if (g_csCj) LOGI("Cutscene: its CJ made of the skin (model %d%s)", skin, g_csShoulders ? ", its shoulders turned as CJ's" : "");
    } else if (o == g_csCj) {
        g_csCj = nullptr;  // the place of a CJ deleted this very frame, given to an object of the next cutscene
    }
    return o;
}

// The scene's animation of CJ is bound to bones by their place in the model the scene's file names, CSPLAY
// (CAnimBlendAssocGroup::CreateAssociations at 0x4CE3B0 makes a clump of that model and CAnimBlendStaticAssociation::Init
// notes each track at the place of its bone there; the object's copy takes the tracks place by place). CSPLAY has 61
// bones and the game's people 32, in another order after the first six: on a skin the head and the spine were right
// and every arm and leg got another bone's track (11.11: the hands above the head in SMOKE1A, the body folded). So for
// the CJ made of a skin the tracks are bound anew to the skin's own clump, by the bones' numbers, before the game
// copies them. Bones the skin has not (the face's 5001-5026, the thumbs) lose their tracks. The tracks of 301 and 302
// are left out unless the skin is rigged as CJ: his are helpers at the upper arms, the others' lie on the chest.
using SetCsAnimFn = void(__cdecl*)(const char*, void*);
SetCsAnimFn g_origSetCsAnim = nullptr;
void __cdecl HookSetCutsceneAnim(const char* name, void* object) {
    auto* clump = object && object == g_csCj ? Field<uint8_t*>(object, 0x18) : nullptr;
    auto* data = clump && clump[0] == 2 ? Field<uint8_t*>(clump, Ref<uint32_t>(kClumpOffset)) : nullptr;
    // CCutsceneMgr::ms_cutsceneAssociations.GetAnimation: the CAnimBlendStaticAssociation the game is about to copy
    void* tracks = data && name ? CallMethod<0x4CE040, void*>((void*)0xBC4020, name) : nullptr;
    void* hier = tracks ? Field<void*>(tracks, 0x10) : nullptr;
    if (hier) {
        CallMethod<0x4CE9A0>(tracks);                     // FreeSequenceArray: the places of CSPLAY's bones
        CallMethod<0x4CEC20>(tracks, (void*)clump, hier);  // Init: a new array, each track at its bone's place in this clump
        auto** seqs = Field<void**>(tracks, 0x0C);
        int n = Field<int16_t>(tracks, 0x04), kept = 0;
        for (uint32_t bone : {301u, 302u}) {
            auto* fd = g_csShoulders ? nullptr : Call<0x4D6400, uint8_t*>((void*)clump, bone);  // RpAnimBlendClumpFindBone
            int i = fd ? (int)((fd - Field<uint8_t*>(data, 0x10)) / 0x18) : -1;                  // AnimBlendFrameData
            if (seqs && i >= 0 && i < n) seqs[i] = nullptr;
        }
        for (int i = 0; seqs && i < n; ++i) kept += seqs[i] != nullptr;
        LOGI("Cutscene: its CJ's animation bound to the skin's %d bones (%d tracks)", n, kept);
    }
    g_origSetCsAnim(name, object);
}

using PreRenderFn = void(__fastcall*)(void*, void*);
PreRenderFn g_origCsPreRender = nullptr;
void __fastcall HookCsPreRender(void* o, void* edx) {
    bool cj = o && o == g_csCj;
    if (cj) WriteFace(o);
    g_origCsPreRender(o, edx);  // the hierarchy's matrices
    if (!cj || !g_csShoulders) return;
    if (void* clump = Field<void*>(o, 0x18)) Call<0x5DF560>(clump);  // CPed::ShoulderBoneRotation, as CSPLAY's
    Field<uint32_t>(o, 0x1C) |= 0x00800000;  // CEntity::m_bDontUpdateHierarchy: the turned shoulders stay this frame
}

uint64_t g_retryMs = 0;

class Skins : public Module {
public:
    const char* Name() const override { return "Skins"; }

    void OnInstallHooks() override {
        mem::HookFn(0x5B02A0, &HookCreateCutsceneObject, &g_origCreateCsObject, "CCutsceneMgr::CreateCutsceneObject");
        mem::HookFn(0x5B0390, &HookSetCutsceneAnim, &g_origSetCsAnim, "CCutsceneMgr::SetCutsceneAnim");
        g_origCsPreRender = (PreRenderFn)mem::SwapVTable((void**)0x868A60, 17, (void*)&HookCsPreRender);  // CCutsceneObject
    }

    void OnPostProcess() override {
        // Forgotten only with the cutscene's data: till 11.11 this looked at ms_running, false while the cutscene loads,
        // so the CJ made of a skin was forgotten before the first frame and nothing below ever ran (a jaw with no pose)
        if (!CutsceneLoaded()) g_csCj = nullptr;
        else if (g_csCj && CutsceneOn()) UpdateTalk();
        // the chosen skin on again (a loaded game made him CJ): only over CJ, another mod's skin is left alone
        CPlayerPed* me = !Cfg().skin.empty() && core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
        if (!me || me->ModelIndex() != 0 || NowMs() < g_retryMs || !WhyNot().empty()) return;
        g_retryMs = NowMs() + 2000;
        int want = ChosenModel();
        if (want <= 0) return;  // CJ, or an own skin gone from the folder (it is put on again once it is back)
        if (Players::SetModel(me, want)) LOGI("Skin %d put on again", want);
        else LOGW("Skin %d could not be put on", want);
    }

    // the own skins' clumps are skinned and outlive CGame::Shutdown (skin::LetAllGo): out before the engine stops
    void OnGameExit() override { skin::LetAllGo(); }
};
Skins g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

const char* GroupName(Group g) {
    switch (g) {
        case MEN: return TR("Мужчины", "Men");
        case WOMEN: return TR("Женщины", "Women");
        case GANGS: return TR("Банды", "Gangs");
        case SERVICE: return TR("Служба", "Service");
        case PACKS: return TR("Паки", "Packs");
        case OWN: return TR("Свои", "Own");
        default: return "";
    }
}

void Rescan() {
    if (!g_standard) return;  // List() reads the folders with the rest
    g_list.resize(g_standard);
    skin::OwnNames(true);
    AddPacked();
    for (const std::string& name : skin::OwnNames()) g_list.push_back({0, OWN, name, name});
}

const std::vector<Skin>& List() {
    if (g_list.empty() && IsPedModel(kFirst)) {  // the model infos are made with the game
        for (int m = kFirst; m <= kLast; ++m)
            if (IsPedModel(m)) g_list.push_back({m, GroupOf(m), NameOf(m)});
        g_standard = g_list.size();
        AddPacked();
        for (const std::string& name : skin::OwnNames()) g_list.push_back({0, OWN, name, name});
    }
    return g_list;
}

int ModelOf(const Skin& s, std::string* why) { return s.file.empty() ? s.model : skin::OwnModel(s.file, why); }

bool IsChosen(const Skin& s) { return s.file.empty() ? s.model && s.model == StandardChosen() : s.file == OwnChosen(); }

bool AnyChosen() { return StandardChosen() || !OwnChosen().empty(); }

void LetUnusedGo() { skin::LetUnusedGo(OwnChosen()); }

std::string WhyNot() {
    CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
    if (!me) return TR("Игра ещё не началась.", "The game has not started yet.");
    if (CutsceneOn()) return TR("Сейчас идёт катсцена.", "A cutscene is on.");
    if (GetPad(0)->DisablePlayerControls) return TR("Сейчас CJ управляет игра.", "The game controls CJ right now.");
    if (me->InVehicle() || me->AttachedTo()) return TR("Сначала выйдите из транспорта.", "Get out of the vehicle first.");
    if (me->Health() <= 0.0f || !(me->PedFlags() & 1)) return TR("CJ должен стоять на земле.", "CJ must stand on the ground.");  // bIsStanding
    return "";
}

bool Wear(const Skin* s, std::string& why) {
    why = WhyNot();
    if (!why.empty()) return false;
    CPlayerPed* me = FindPlayerPed(0);
    if (!s) {
        if (me->ModelIndex() != 0) {
            Players::SetModel(me, 0);
            Call<0x5A82C0>(me, false);  // CClothes::RebuildPlayer: his own clothes, fat and muscle from the stats
        }
        Keep("");
        LOGI("Skin: CJ again");
        return true;
    }
    int model = ModelOf(*s, &why);
    if (model <= 0) return false;
    if (!Players::SetModel(me, model)) return why = TR("Модель не загрузилась.", "The model did not load."), false;
    Keep(s->file.empty() ? std::to_string(model) : s->file + ".dff");
    LOGI("Skin %d (%s) worn", model, s->name.c_str());
    return true;
}

}  // namespace coop::skins
