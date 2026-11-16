// The players' own skins and the mod's skin packs (OwnSkins.h), a players' wish (09.11), the packs the user's (10.11).
//  * Keys: an own skin's file name, a pack's "pack/name" ('/' is in no file name). The keys come from the folders read
//    here: a key from the network is looked up among them, no path is made of it.
//  * Its model number: one this game has no model info and no disc entry for, from 19999 down, kept for that name while
//    the game runs. Written into CModelInfo::ms_modelInfoPtrs only while the game reads that table at its place
//    (CStreaming::RemoveModel's mov at 0x4089CF): a limit adjuster that moved it leaves own skins off.
//  * Its model info: a copy of a ped's of the game (the one the file is named after, else male01: the walk, voice and
//    stats the game gives a ped of that model) with a key, a txd slot, a clump and a hit model of its own. The copied hit
//    model is the other model's and goes with it (CPedModelInfo::DeleteRwObject), SetClump makes ours.
//  * Loaded as the streaming loads a ped (CStreaming::ConvertBufferToObject): the txd read into its slot and made
//    current, the clump read and set (CPedModelInfo::SetClump). Its streaming entry says loaded, in no list, and the model
//    info holds a reference of ours: CStreaming::ReInit (a load, a new game) removes every model from 1000 on that nothing
//    uses, RemoveLeastUsedModel any in the loaded list without one. Asked for from the disc it would be read with size 0,
//    so the mod's own requests skip it (Players::SetModel, the preview).
//  * Checked before the game gets it: skinned, with each bone the game looks up by number in a ped (its nodes,
//    RpAnimBlendClumpFillFrameArray, and its hit spheres, CPedModelInfo::m_pColNodeInfos), else a broken file crashes the
//    first frame it is drawn.
//  * Let go when nothing uses it (LetUnusedGo): out of the table, its txd emptied, the number and the slot kept for it.
#include "entities/skin/OwnSkins.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"

namespace coop::skin {

namespace {
using namespace game;

constexpr size_t kMaxSkins = 64, kMaxPacked = 512;  // the mod's packs hold 266 since 12.11
constexpr size_t kMaxFileBytes = 64u << 20;
constexpr int kTemplate = 7;  // male01
// pelvis, spine, spine1, neck, head, jaw, both arms from the clavicle to the hand, both legs from the thigh to the foot
constexpr int kBones[] = {1, 2, 3, 4, 5, 8, 21, 22, 23, 24, 31, 32, 33, 34, 41, 42, 43, 51, 52, 53};

struct Own {
    int model = -1;           // its number here, kept once given
    int txd = -1;             // its CTxdStore slot, kept once made
    uint8_t* info = nullptr;  // its CPedModelInfo (0x44 bytes)
    bool loaded = false;
    std::string failed;       // why it did not load: not tried again until the folder is read anew
};
std::map<std::string, Own> g_own;  // by key: an own skin's name, a pack's "pack/name"
std::vector<std::string> g_names, g_packed;
bool g_scanned = false;

// A skin's own line of the game's peds.ide (its folder's peds.ide): who it is, as its authors gave it
struct Def {
    std::string type, walk, voiceType, voiceMin, voiceMax;
};
std::map<std::string, Def> g_defs;  // by key

struct RwMemory {
    uint8_t* start;
    uint32_t length;
};

std::wstring Folder() { return Cfg().coopDir + L"skins\\"; }
std::wstring PacksFolder() { return Cfg().coopDir + L"skinpacks\\"; }

// a key's files without the extension
std::wstring PathOf(const std::string& key) {
    size_t slash = key.find('/');
    return slash == std::string::npos ? Folder() + Utf8ToWide(key)
                                      : PacksFolder() + Utf8ToWide(key.substr(0, slash)) + L"\\" + Utf8ToWide(key.substr(slash + 1));
}
std::string NameOf(const std::string& key) { return key.substr(key.find('/') + 1); }  // npos + 1: all of it

void*& InfoSlot(int model) { return Ref<void*>(ModelInfoTable() + (uintptr_t)model * 4); }


std::string Lower(std::wstring w) {
    CharLowerBuffW(w.data(), (DWORD)w.size());  // one name in every game, whatever the case of its file
    return WideToUtf8(w);
}

// a folder's pairs (a .dff with a .txd of its name), by name in lower case, each with prefix before it
void ScanFolder(const std::wstring& dir, const std::string& prefix, std::vector<std::string>& into) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.dff").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring file = fd.cFileName;
        // the mask also takes a longer extension by its short name
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || file.size() <= 4 || _wcsicmp(file.c_str() + file.size() - 4, L".dff"))
            continue;
        std::wstring base = file.substr(0, file.size() - 4);
        DWORD txd = GetFileAttributesW((dir + base + L".txd").c_str());
        std::string key = prefix + Lower(base);
        if (txd == INVALID_FILE_ATTRIBUTES || (txd & FILE_ATTRIBUTE_DIRECTORY) || key.size() == prefix.size() || key.size() > 64) continue;
        into.push_back(key);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// A folder's peds.ide: the packs' authors give their skins' lines as the game's file has them ("6812, VLA15, VLA15,
// CIVFEMALE, STAT_STREET_GIRL, woman, 1983, 1, null, 0, 3, PED_TYPE_GEN, VOICE_GEN_HFYST, VOICE_GEN_HFYST"), and one
// may go next to one's own skins. Its words are only compared with the game's names, nothing else is made of them.
void ReadDefs(const std::wstring& dir, const std::string& prefix) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(dir + L"peds.ide", bytes) || bytes.size() > (1u << 20)) return;
    for (std::string line : Split(std::string(bytes.begin(), bytes.end()), '\n')) {
        line = line.substr(0, line.find('#'));
        for (char& c : line)
            if (c == ',' || c == '\t' || c == '\r') c = ' ';
        std::vector<std::string> w;
        for (const std::string& word : Split(line, ' '))
            if (!word.empty()) w.push_back(word);
        bool plain = w.size() >= 14;
        for (const std::string& word : w)
            plain = plain && word.size() < 32 &&
                    std::all_of(word.begin(), word.end(), [](char c) { return isalnum((unsigned char)c) || c == '_'; });
        if (plain) g_defs[prefix + ToLower(w[1])] = {w[3], w[5], w[11], w[12], w[13]};
    }
}

void Scan() {
    g_scanned = true;
    g_names.clear(), g_packed.clear(), g_defs.clear();
    for (auto& [name, o] : g_own) o.failed.clear();  // the files may be fixed
    CreateDirectoryW(Folder().c_str(), nullptr);  // the window names it: it is there to put files into
    ScanFolder(Folder(), "", g_names);
    ReadDefs(Folder(), "");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((PacksFolder() + L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') {
                ScanFolder(PacksFolder() + fd.cFileName + L"\\", Lower(fd.cFileName) + "/", g_packed);
                ReadDefs(PacksFolder() + fd.cFileName + L"\\", Lower(fd.cFileName) + "/");
            }
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (auto* v : {&g_names, &g_packed}) {
        std::sort(v->begin(), v->end());
        v->erase(std::unique(v->begin(), v->end()), v->end());
    }
    if (g_names.size() > kMaxSkins) {
        LOGW("Own skins: %zu in the folder, the first %zu are listed", g_names.size(), kMaxSkins);
        g_names.resize(kMaxSkins);
    }
    if (g_packed.size() > kMaxPacked) {
        LOGW("Skin packs: %zu skins, the first %zu are listed", g_packed.size(), kMaxPacked);
        g_packed.resize(kMaxPacked);
    }
}

bool FreeNumber(int m) {
    const uint8_t* si = StreamingInfoOf(m);
    if (InfoSlot(m) || !si || Field<int16_t>(si, 0) != -1 || Field<int16_t>(si, 2) != -1 || si[6] || Field<uint32_t>(si, 0x0C) ||
        si[0x10])
        return false;
    for (auto& [name, o] : g_own)
        if (o.model == m) return false;
    return true;
}

// BoPoH's First Person Mod (FirstPerson.sp, loaded by its Hooks.asi; its 3.0 and MixMods' 3.0 Fixed alike) keeps a camera
// offset for each skin in a block of its own of 0x3D4C bytes, 12 bytes a ped model from +12000: the numbers under 300
// only. It reads the player's row each frame and ties its settings' sliders (Alt+B) to it once the player's model
// changes: with an own skin's number from 19999 down it read and wrote some 240 KB past its block, and the partners who
// put on an own skin with it crashed (13.11, a broken heap: the next restart fell in RpMaterialDestroy). With it loaded,
// own skins take numbers under 290 the game leaves empty (290-299 are the special characters', 1 is the cutscene CJ's,
// 2 and 3 mean something to the cutscene loader) and give them back when let go: they are few.
bool FirstPersonMod() { return GetModuleHandleA("FirstPerson.sp") != nullptr; }

int FreeModel() {
    if (FirstPersonMod()) {
        for (int m = 4; m < 290; ++m)
            if (FreeNumber(m)) return m;
        LOGW("Own skins: no free model number under 290, which the First Person mod needs");
        return -1;
    }
    for (int m = ModelCount() - 1; m >= 1000; --m)
        if (FreeNumber(m)) return m;
    return -1;
}

bool TxdRoom() {
    auto* pool = Ref<uint8_t*>(0xC8800C);  // CTxdStore::ms_pTxdPool: New() of a full one gives null, AddTxdSlot writes to it
    auto* flags = pool ? Field<uint8_t*>(pool, 4) : nullptr;
    int n = pool ? Field<int32_t>(pool, 8) : 0;
    for (int i = 0; flags && i < n; ++i)
        if (flags[i] & 0x80) return true;
    return false;
}

// The file's chunks walked before RenderWare reads them: a vehicle's file has its collision in the clump's extension, and
// the game's collision plugin reads it into the scratch pad and gives it to the model being loaded
// (CCollisionPlugin::ms_currentModel), none here: a car's file crashed the stand writing to address 0x14 (10.11). A
// person's file has none. Empty: the first clump is fine to read.
std::string ChunkTrouble(const std::vector<uint8_t>& b) {
    auto at32 = [&](size_t at) {
        uint32_t v;
        memcpy(&v, b.data() + at, 4);
        return v;
    };
    // the chunks from..to; false: one runs past the end
    auto walk = [&](size_t from, size_t to, auto&& each) {
        while (from + 12 <= to) {
            uint32_t type = at32(from), size = at32(from + 4);
            if (size > to - from - 12) return false;
            if (!each(type, from + 12, from + 12 + size)) return true;
            from += 12 + size;
        }
        return true;
    };
    std::string trouble = "no clump";
    bool whole = walk(0, b.size(), [&](uint32_t type, size_t from, size_t to) {
        if (type != 0x10) return true;  // rwID_CLUMP: the first is read
        trouble.clear();
        bool inside = walk(from, to, [&](uint32_t t, size_t f, size_t e) {
            bool ok = t != 3 || walk(f, e, [&](uint32_t et, size_t, size_t) {  // rwID_EXTENSION
                if (et == 0x253F2FA) trouble = "collision";              // rwID_COLLISIONPLUGIN
                return trouble.empty();
            });
            if (!ok) trouble = "broken";
            return trouble.empty();
        });
        if (!inside) trouble = "broken";
        return false;
    });
    return whole ? trouble : "broken";
}

std::string WhatIsWrong(void* clump) {
    if (!Call<0x4C4DC0, bool>(clump)) return "no skin";  // IsClumpSkinned
    void* hier = Call<0x734B10, void*>(clump);           // GetAnimHierarchyFromClump
    if (!hier) return "no bones";
    for (int b : kBones)
        if (Call<0x7C51A0, int>(hier, b) < 0) return Format("no bone %d", b);  // RpHAnimIDGetIndex
    return "";
}

void* ReadClump(std::vector<uint8_t>& bytes, int txd) {
    Call<0x7316A0>();     // CTxdStore::PushCurrentTxd
    Call<0x7319C0>(txd);  // CTxdStore::SetCurrentTxd: its materials find their textures there
    RwMemory mem{bytes.data(), (uint32_t)bytes.size()};
    void* s = Call<0x7ECEF0, void*>(3, 1, &mem);  // RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD)
    void* clump = s && Call<0x7ED2D0, int>(s, 0x10, nullptr, nullptr) ? Call<0x74B420, void*>(s) : nullptr;  // rwID_CLUMP
    if (s) Call<0x7ECE20, int>(s, &mem);  // RwStreamClose
    Call<0x7316B0>();     // CTxdStore::PopCurrentTxd
    return clump;
}

bool ReadTxd(std::vector<uint8_t>& bytes, int txd) {
    RwMemory mem{bytes.data(), (uint32_t)bytes.size()};
    void* s = Call<0x7ECEF0, void*>(3, 1, &mem);
    bool ok = s && Call<0x731DD0, bool>(txd, s);  // CTxdStore::LoadTxd
    if (s) Call<0x7ECE20, int>(s, &mem);
    return ok;
}

// a CPedModelInfo of the game's own class: copied whole, a mod's bigger class would be read past its end
bool PedInfoToCopy(int model) { return IsPedModel(model) && Field<uintptr_t>(InfoSlot(model), 0) == 0x85BDC0; }

int PedTypeOf(const std::string& name) {  // CPedType::FindPedType's names but the players'
    static const char* const kTypes[] = {"CIVMALE", "CIVFEMALE", "COP",     "GANG1",    "GANG2", "GANG3",      "GANG4",
                                         "GANG5",   "GANG6",     "GANG7",   "GANG8",    "GANG9", "GANG10",     "DEALER",
                                         "MEDIC",   "FIREMAN",   "CRIMINAL", "BUM",     "PROSTITUTE", "SPECIAL"};
    for (int i = 0; i < (int)std::size(kTypes); ++i)
        if (!_stricmp(name.c_str(), kTypes[i])) return 4 + i;  // ePedType: PED_TYPE_CIVMALE is 4
    return -1;
}

// The skin's own line over the copied ped's (11.11, the players' wish: a girl walked and spoke as male01): its sex and
// voice, which CAEPedSpeechAudioEntity::Initialise reads, and its walk set (CPlayerPed::ProcessAnimGroups), each made a
// number as CFileLoader::LoadPedObject makes it. A word this game does not know keeps the copied ped's.
void Define(uint8_t* info, const std::string& name) {
    auto it = g_defs.find(name);
    if (it == g_defs.end()) return;
    const Def& d = it->second;
    if (int type = PedTypeOf(d.type); type >= 0) Field<int32_t>(info, 0x28) = type;  // m_nPedType
    int walk = WalkGroupByName(d.walk.c_str());
    if (PedWalkGroup(walk)) Field<int32_t>(info, 0x24) = walk;  // m_nAnimType
    int16_t audio = Call<0x4E3C60, int16_t>(d.voiceType.c_str());  // CAEPedSpeechAudioEntity::GetAudioPedType: -1 for none
    // GEN, EMG, GANG, GFD: not the player's, nor a special one's (found by the model's key)
    bool typed = audio == 0 || audio == 1 || audio == 3 || audio == 4;
    int16_t lo = typed ? Call<0x4E3CD0, int16_t>(d.voiceMin.c_str(), (int)audio) : (int16_t)-1;  // GetVoice
    int16_t hi = lo >= 0 ? Call<0x4E3CD0, int16_t>(d.voiceMax.c_str(), (int)audio) : (int16_t)-1;
    if (hi >= 0) {  // max below min: IncrementVoice keeps to min
        Field<int16_t>(info, 0x3C) = audio;  // m_nPedAudioType
        Field<int16_t>(info, 0x3E) = lo;     // m_nVoiceMin
        Field<int16_t>(info, 0x40) = hi;     // m_nVoiceMax
        Field<int16_t>(info, 0x42) = lo;     // m_nVoiceId
    }
    LOGI("Own skin %s as its line says: %s, walk %s%s, voice %s%s", name.c_str(), d.type.c_str(), d.walk.c_str(),
         PedWalkGroup(walk) ? "" : " (not one of ped.ifp's: the copied ped's)", d.voiceMin.c_str(),
         hi >= 0 ? "" : " (unknown: the copied ped's)");
}

bool Load(const std::string& name, Own& o, std::string& why) {
    if (!ModelTablesKnown()) return why = TR("Другой мод изменил код моделей игры.", "Another mod changed the game's model code."), false;
    int from = StandardModel(name);
    if (!PedInfoToCopy(from)) from = kTemplate;
    if (!PedInfoToCopy(from)) return why = TR("Другой мод изменил модели людей игры.", "Another mod changed the game's people models."), false;
    std::vector<uint8_t> dff, txd;
    std::wstring base = PathOf(name);
    if (!ReadFileBytes(base + L".dff", dff) || !ReadFileBytes(base + L".txd", txd) || dff.empty() || txd.empty())
        return why = TR("Файлы скина не читаются.", "The skin's files cannot be read."), false;
    if (dff.size() > kMaxFileBytes || txd.size() > kMaxFileBytes) return why = TR("Файлы скина слишком большие.", "The skin's files are too big."), false;
    if (std::string trouble = ChunkTrouble(dff); !trouble.empty()) {
        LOGW("Own skin %s: not a person's (%s)", name.c_str(), trouble.c_str());
        return why = TR("Это не скин персонажа.", "This is not a person's skin."), false;
    }
    if (o.model < 0 && (o.model = FreeModel()) < 0) return why = TR("В игре нет свободных номеров моделей.", "The game has no free model numbers."), false;
    if (o.txd < 0) {
        if (!TxdRoom()) return why = TR("В игре нет места для текстур.", "The game has no room for textures."), false;
        o.txd = Call<0x731C80, int>(("coop_" + name).c_str());  // CTxdStore::AddTxdSlot
    }
    if (!ReadTxd(txd, o.txd)) {
        Call<0x731E90>(o.txd);  // CTxdStore::RemoveTxd
        LOGW("Own skin %s: its txd is not one", name.c_str());
        return why = TR("Файл .txd повреждён.", "The .txd file is broken."), false;
    }
    void* clump = ReadClump(dff, o.txd);
    std::string wrong = clump ? WhatIsWrong(clump) : "no model";
    if (!wrong.empty()) {
        if (clump) Call<0x74A310>(clump);  // RpClumpDestroy
        Call<0x731E90>(o.txd);
        LOGW("Own skin %s: not a person's (%s)", name.c_str(), wrong.c_str());
        return why = TR("Это не скин персонажа.", "This is not a person's skin."), false;
    }
    if (!o.info) o.info = new uint8_t[0x44];
    memcpy(o.info, InfoSlot(from), 0x44);
    Field<uint32_t>(o.info, 0x04) = Call<0x53CF30, uint32_t>(("coop_" + name).c_str());  // m_nKey: not found by the game's names
    Field<uint16_t>(o.info, 0x08) = 1;   // m_nRefCount: ours, while it is loaded
    Field<int16_t>(o.info, 0x0A) = (int16_t)o.txd;
    Field<uint8_t>(o.info, 0x0D) = 0;    // m_n2dfxCount
    Field<void*>(o.info, 0x1C) = nullptr;  // m_pRwObject
    Field<int32_t>(o.info, 0x20) = -1;   // m_nAnimFileIndex
    Field<void*>(o.info, 0x34) = nullptr;  // m_pHitColModel
    Define(o.info, name);
    CallMethod<0x4C7340>(o.info, clump);   // CPedModelInfo::SetClump
    InfoSlot(o.model) = o.info;
    StreamingInfoOf(o.model)[0x10] = 1;  // LOADSTATE_LOADED
    o.loaded = true;
    LOGI("Own skin %s loaded as model %d (txd %d, like model %d)", name.c_str(), o.model, o.txd, from);
    return true;
}

void Unload(const std::string& name, Own& o) {
    uint8_t* si = StreamingInfoOf(o.model);
    // a SetModelIsDeletable of its number puts it into the loaded list, where RemoveLeastUsedModel would read the model info
    // this takes out of the table
    if (Field<int16_t>(si, 0) != -1) CallMethod<0x4074E0>(si);  // CStreamingInfo::RemoveFromList
    CallVirtual<8>(o.info);  // CPedModelInfo::DeleteRwObject: the clump, its txd reference, its hit model
    si[6] = 0, si[0x10] = 0;  // no flags, not loaded
    InfoSlot(o.model) = nullptr;
    Call<0x731E90>(o.txd);   // CTxdStore::RemoveTxd: a texture something still holds stays
    o.loaded = false;
    LOGI("Own skin %s let go (model %d)", name.c_str(), o.model);
    if (o.model < 1000) o.model = -1;  // a number under 290 (FreeModel): few, the next skin takes it
}

bool Listed(const std::string& key) {
    if (!g_scanned) Scan();
    const auto& v = key.find('/') == std::string::npos ? g_names : g_packed;
    return std::binary_search(v.begin(), v.end(), key);
}
}  // namespace

const std::vector<std::string>& OwnNames(bool fresh) {
    if (fresh || !g_scanned) Scan();
    return g_names;
}

const std::vector<std::string>& PackSkins() {
    if (!g_scanned) Scan();
    return g_packed;
}

int OwnModel(const std::string& name, std::string* why) {
    std::string reason;
    auto it = g_own.find(name);
    if (it != g_own.end() && it->second.loaded) {
        Own& o = it->second;
        if (ModelLoadState(o.model) == 1 && InfoSlot(o.model) == o.info) return o.model;
        // not seen to happen: the game removed it (no reference of ours kept it) or gave its number away. Loaded anew.
        LOGW("Own skin %s: the game took model %d away (load state %d)", name.c_str(), o.model, ModelLoadState(o.model));
        if (InfoSlot(o.model) == o.info && ModelLoadState(o.model) == 0) InfoSlot(o.model) = nullptr;  // removed: its number again
        else o.model = -1, o.info = nullptr;  // the game holds that number and that info now: both left to it
        Call<0x731E90>(o.txd);  // CTxdStore::RemoveTxd
        o.loaded = false;
    }
    if (!Listed(name)) reason = TR("Такого скина нет в папке coop\\skins.", "No such skin in coop\\skins.");
    else if (it != g_own.end() && !it->second.failed.empty()) reason = it->second.failed;
    else {
        Own& o = g_own[name];
        if (Load(name, o, reason)) return o.model;
        o.failed = reason;
    }
    if (why) *why = reason;
    return -1;
}

std::string OwnName(int model) {
    for (auto& [name, o] : g_own)
        if (o.loaded && o.model == model) return name;
    return "";
}

bool IsOwn(int model) {
    for (auto& [name, o] : g_own)
        if (o.model == model) return model >= 0;
    return false;
}

int StandardModel(const std::string& key) {
    std::string name = NameOf(key);
    int index = -1;
    void* info = name.empty() ? nullptr : Call<0x4C5940, void*>(name.c_str(), &index);  // CModelInfo::GetModelInfo by name
    return info && index >= 7 && index <= 288 && IsPedModel(index) ? index : -1;
}

void LetUnusedGo(const std::string& keep) {
    for (auto& [name, o] : g_own)
        if (o.loaded && name != keep && Field<uint16_t>(o.info, 0x08) == 1 && ModelLoadState(o.model) == 1) Unload(name, o);
}

// CModelInfo::ShutDown shuts the model infos of the game's stores only: ours kept its clump past CGame::Shutdown, and
// RwEngineStop destroyed it after the skin plugin had closed (the closing game's crash at 0x801D58, 13.11)
void LetAllGo() {
    for (auto& [name, o] : g_own)
        if (o.loaded && InfoSlot(o.model) == o.info) Unload(name, o);
}

}  // namespace coop::skin
