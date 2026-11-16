// The voice of a skin in the story's lines outside the cutscenes (StoryVoice.h), the user's wish (11.11): a mission's
// dialogue and the phone calls are single recorded lines (audio\SFX\SCRIPT, 200 a bank), and CJ's stayed CJ's.
//  * The files: coop\voices\female\lines.pak and coop\voices\male\lines.pak hold CJ's lines said by another voice,
//    coop\voices\names.pak the others' lines with his name cut out. Each line is as long as the game's own (the same
//    bytes at the same rate; a cut line ends with silence), so the scripts that wait for a line to end wait as long in
//    every game. They are made outside the mod (the tests' scripts/analysis/voice) and are not in its archive.
//  * Given to the game after it loaded the line: the loader (CAEMP3BankLoader::Service, 0x4DFE30) reads a single sound
//    into its bank slot's part of the buffer and marks the slot (its bank, m_BankSlotSound), and a sound is played from
//    there (CAEAudioHardware::PlaySound takes GetSoundBuffer's). Once a slot's line changed, ours is copied over it:
//    nothing else is touched, and a line of another length or rate (a game with other sound files) is left alone.
//  * The slots are looked at before and after the game's frame, not in the loader: modloader (std.bank) puts its own
//    loader in the game's place, which fills the same slots, and a hook of Service never ran there (11.11). A script
//    plays a line a frame after it saw it loaded, and the sound starts in the audio service that follows the frame
//    (Idle: CGame::Process, then CAudioEngine::Service), so a look on each side of the frame is always before it.
//  * Whose line: the story's CJ is our own player, and in a partner's game the host for the lines the host's missions
//    play there (MissionSpeech tells the slot). A skin is a woman's or a man's by its ped type, as in the cutscenes.
//  * The subtitles: a dialogue line is shown without his name while names.pak is there and that player wears a skin
//    (SceneLines.h); Missions shows the lines.
//  * Each game chooses for itself and the lines are as long as the game's: nothing goes over the network.
//  * A line already in a slot when the skin changes is said once more as it was loaded.
#include "entities/skin/StoryVoice.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/skin/SceneLines.h"
#include "entities/skin/SceneVoice.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"

namespace coop::skin {

namespace {
using namespace game;

constexpr int kFirstBank = 147;              // SND_BANK_SCRIPT_FIRST
constexpr int kFirstLine = 2000, kPerBank = 200, kBanks = 400;  // a line's id: 2000 + 200 * (bank - 147) + sound
constexpr int kMissionSlot = 26, kSlots = 60;  // SND_BANK_SLOT_MISSION1 (four of them); m_BankSlotSound's size
constexpr uint32_t kMaxLines = 8192, kMaxBytes = 8u << 20;

// A pack: "SCLN", version 1, the count, a spare word; then the lines by id (id, offset in the file, bytes, rate), then
// their sound: 16-bit mono as the game's banks have it
struct Line {
    uint32_t id, offset, bytes, rate;
};
struct Pack {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::vector<Line> lines;

    const Line* Find(uint32_t id) const {
        auto it = std::lower_bound(lines.begin(), lines.end(), id, [](const Line& l, uint32_t v) { return l.id < v; });
        return it != lines.end() && it->id == id ? &*it : nullptr;
    }
    bool Read(void* to, uint32_t at, uint32_t bytes) const {
        LARGE_INTEGER pos;
        pos.QuadPart = at;
        DWORD got = 0;
        return SetFilePointerEx(file, pos, nullptr, FILE_BEGIN) && ReadFile(file, to, bytes, &got, nullptr) && got == bytes;
    }
    // The file is another's: every number in it is checked before a line of it can reach the game's buffer
    void Open(const std::wstring& path, const char* name) {
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {  // no such file: most games
            DWORD err = GetLastError();
            if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) LOGW("Story voice: %s could not be opened (%lu)", name, err);
            return;
        }
        const char* why = nullptr;
        LARGE_INTEGER size{};
        uint32_t head[4] = {};
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 16 || size.QuadPart > 0x7FFFFFFF) why = "its size";
        else if (!Read(head, 0, 16) || memcmp(head, "SCLN", 4) != 0 || head[1] != 1) why = "not a lines pack";
        else if (head[2] == 0 || head[2] > kMaxLines || 16 + (uint64_t)head[2] * 16 > (uint64_t)size.QuadPart) why = "its count";
        if (!why) {
            lines.resize(head[2]);
            uint32_t data = 16 + head[2] * 16, last = 0;
            if (!Read(lines.data(), 16, head[2] * 16)) why = "its index";
            for (const Line& l : lines) {
                if (why) break;
                bool known = l.id >= (uint32_t)kFirstLine && l.id < (uint32_t)(kFirstLine + kPerBank * kBanks) && l.id > last;
                bool sound = l.bytes >= 2 && l.bytes % 2 == 0 && l.bytes <= kMaxBytes && l.rate >= 4000 && l.rate <= 48000;
                bool inside = l.offset >= data && (uint64_t)l.offset + l.bytes <= (uint64_t)size.QuadPart;
                if (!known || !sound || !inside) why = "a line of its index";
                last = l.id;
            }
        }
        if (why) {
            LOGW("Story voice: %s is left alone (%s)", name, why);
            lines.clear();
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
        }
    }
};
Pack g_female, g_male, g_names;
bool g_read = false;         // the packs were looked for
bool g_hosts[4] = {};        // a mission audio slot holds a line of the host's game
uint32_t g_seen[kSlots];     // a bank slot's (bank << 16) | sound at the last look
std::vector<uint8_t> g_bytes;

void ReadPacks() {
    g_read = true;
    std::fill(std::begin(g_seen), std::end(g_seen), 0xFFFFFFFFu);
    std::wstring dir = Cfg().coopDir + L"voices\\";
    g_female.Open(dir + L"female\\lines.pak", "female\\lines.pak");
    g_male.Open(dir + L"male\\lines.pak", "male\\lines.pak");
    g_names.Open(dir + L"names.pak", "names.pak");
    if (!g_female.lines.empty() || !g_male.lines.empty() || !g_names.lines.empty())
        LOGI("Story voice: %zu lines of a woman's voice, %zu of a man's, %zu of the others without the name",
             g_female.lines.size(), g_male.lines.size(), g_names.lines.size());
    else
        LOGD("Story voice: no packs in %s", WideToUtf8(dir).c_str());
}

// The story's CJ for a line: 0 while that player is CJ himself
int StoryCjModel(bool hosts) {
    CPed* cj = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
    if (hosts && Net().Online() && !Net().IsHost()) {
        const RemotePlayer* host = Players::Instance().Get(Net().HostId());
        cj = host ? host->ped : nullptr;
    }
    int skin = cj ? cj->ModelIndex() : 0;
    return skin != 0 && IsPedModel(skin) ? skin : 0;
}

// Memory a line can be written to: committed and writable all the way
bool Writable(const uint8_t* at, size_t bytes) {
    for (const uint8_t* end = at + bytes; at < end;) {
        MEMORY_BASIC_INFORMATION about;
        if (!VirtualQuery(at, &about, sizeof(about)) || about.State != MEM_COMMIT) return false;
        if (!(about.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) || (about.Protect & PAGE_GUARD))
            return false;
        at = (const uint8_t*)about.BaseAddress + about.RegionSize;
    }
    return true;
}

// The lines the loader finished since the last look, replaced where a pack has them
void Replace(void* loader) {
    if (!Field<uint8_t>(loader, 0x14)) return;  // m_IsInitialised
    if (!g_read) ReadPacks();
    if (g_female.lines.empty() && g_male.lines.empty() && g_names.lines.empty()) return;
    auto* slots = Field<uint8_t*>(loader, 0x00);   // CAEBankSlot[], 0x12D4 bytes each
    int n = std::min<int>(Field<uint16_t>(loader, 0x0C), kSlots);
    const int16_t* single = &Field<int16_t>(loader, 0x66C);  // m_BankSlotSound: the one sound a slot holds, -1 a whole bank
    for (int i = 0; slots && i < n; ++i) {
        uint8_t* slot = slots + (size_t)i * 0x12D4;
        int bank = Field<int16_t>(slot, 0x10), sound = single[i];
        uint32_t now = ((uint32_t)(uint16_t)bank << 16) | (uint16_t)sound;
        if (now == g_seen[i]) continue;
        g_seen[i] = now;
        // a single sound of a story bank (NumSounds -1). A slot never used has no bank, and under modloader anything
        // in m_BankSlotSound
        if (bank < kFirstBank || bank >= kFirstBank + kBanks || sound < 0 || sound >= kPerBank || Field<int16_t>(slot, 0x12) != -1) continue;
        uint32_t id = (uint32_t)(kFirstLine + kPerBank * (bank - kFirstBank) + sound);
        bool hosts = i >= kMissionSlot && i < kMissionSlot + 4 && g_hosts[i - kMissionSlot];
        int model = StoryCjModel(hosts);
        if (!model) continue;
        const Pack* pack = SkinIsFemale(model) ? &g_female : &g_male;
        const Line* line = pack->Find(id);
        if (!line) line = (pack = &g_names)->Find(id);
        if (!line) continue;
        // Where the game will take the sound from when it plays it, and how much of it (CAEMP3BankLoader::
        // GetSoundBuffer): the loader's buffer plus the slot's offset in the game, a buffer of its own under modloader
        // (the loader's is null there and the offset is the address)
        uint32_t bytes = 0;
        uint16_t rate = 0;
        auto* at = CallMethod<0x4E0280, uint8_t*>(loader, sound, i, &bytes, &rate);
        if (!at || bytes != line->bytes || rate != line->rate || !Writable(at, bytes)) {
            LOGW("Story voice: line %u is left alone (the game's is %u bytes at %u Hz, the pack's %u at %u)", id, bytes, (unsigned)rate,
                 line->bytes, line->rate);
            continue;
        }
        g_bytes.resize(bytes);
        if (!pack->Read(g_bytes.data(), line->offset, bytes)) {
            LOGW("Story voice: line %u could not be read", id);
            continue;
        }
        memcpy(at, g_bytes.data(), bytes);
        LOGD("Story voice: line %u in slot %d is %s (model %d)", id, i, pack == &g_names ? "said without the name" : "the skin's", model);
    }
}

class StoryVoice : public Module {
public:
    const char* Name() const override { return "StoryVoice"; }

    void OnPreProcess() override { Look(); }
    void OnPostProcess() override { Look(); }

private:
    static void Look() {
        if (void* loader = Ref<void*>(0xB5F8B8 + 0xD98)) Replace(loader);  // AEAudioHardware.m_pMP3BankLoader
    }
};
StoryVoice g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;

}  // namespace

void StoryLineFrom(int slot, bool hosts) {
    if (slot >= 0 && slot < 4) g_hosts[slot] = hosts;
}

bool StoryLine(const char* text, bool hosts, std::string& shown) {
    if (!text || g_names.lines.empty() || !StoryCjModel(hosts)) return false;
    shown = WithoutName(text);
    return shown != text;
}

}  // namespace coop::skin
