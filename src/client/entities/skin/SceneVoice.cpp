// The voice of a skin in the cutscenes (SceneVoice.h), the user's wish (11.11): a skin spoke with CJ's voice and the
// others called it CJ.
//  * The files: coop\voices\female\<SCENE>.ogg and coop\voices\male\<SCENE>.ogg, the cutscene's own track (Ogg Vorbis,
//    as long as the game's) with CJ's lines said by another voice and his name wiped from the others' lines. They are
//    made outside the mod (the tests' scripts/analysis/voice) and are not in its archive: a game without a scene's file
//    plays the game's sound.
//  * Chosen as the cutscene asks for its track (CCutsceneMgr::LoadCutsceneData_preload, its call at 0x5B05FC): the
//    scene's CJ is made of a skin (SceneCjModel, the rule features/skins swaps CSPLAY by), the skin is a woman's or a
//    man's by its ped type (peds.ide: CIVFEMALE, PROSTITUTE), and that folder has the scene's file. The call waits
//    there for the stream thread to open the track (CAECutsceneTrackManager::PreloadCutsceneTrack), so the choice is
//    made before it and read by that thread under a lock.
//  * Given to the game's own decoder: CAETrackLoader::GetDataStream (0x4E0D20, on the stream thread) makes a
//    CAEDataStream over a piece of AUDIO\STREAMS\CUTSCENE; for the chosen track ours makes the same object over our
//    file, from its start, its whole length, not encrypted. The game plays, pauses, stops and frees it as its own (the
//    object and its name come from the game's operator new: its destructor deletes them).
//  * Checked before the game gets it: the game waits for a track to be ready without an end (the loop of
//    PreloadCutsceneTrack), so a file its decoder cannot open would hang the cutscene's load. The file is opened here
//    first, from memory, with the game's own Vorbis functions (its imports of vorbisFile.dll, as CAEVorbisDecoder::
//    Initialise at 0x5024D0 calls them): one or two channels, a rate, a length.
//  * The subtitles: while that track plays a line is shown without his name (SceneLines.h); Missions shows the lines.
//  * Each game chooses for itself: the tracks are as long as the game's, so a game without the files is in step with
//    one that has them. Nothing goes over the network.
#include "entities/skin/SceneVoice.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "entities/skin/SceneLines.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop::skin {

namespace {
using namespace game;

std::mutex g_lock;                  // the stream thread reads what the game's thread chose
int g_track = -1;                   // under g_lock: the track our file is given for, -1: none
char g_path[MAX_PATH] = {};         // under g_lock: that file, as CreateFileA takes it
std::atomic<bool> g_opened{false};  // the stream thread opened it for the scene chosen last
std::string g_scene;                // the game's thread: the scene the choice was made for

// CCutsceneMgr::ms_cutsceneName (char[8]) as a file's name: letters, digits and '_' in upper case, empty for any other
std::string SceneName() {
    const char* name = (const char*)0xBC3F0C;
    std::string s(name, strnlen(name, 8));
    for (char& c : s) {
        if (!std::isalnum((unsigned char)c) && c != '_') return std::string();
        c = (char)std::toupper((unsigned char)c);
    }
    return s;
}

// a file in memory for the Vorbis functions (ov_callbacks: read, seek, close, tell)
struct Bytes {
    const uint8_t* data;
    size_t size, at;
};
size_t __cdecl ReadBytes(void* to, size_t size, size_t count, void* from) {
    auto* b = (Bytes*)from;
    size_t n = size ? std::min(count, (b->size - b->at) / size) : 0;
    memcpy(to, b->data + b->at, n * size);
    b->at += n * size;
    return n;
}
int __cdecl SeekBytes(void* from, int64_t offset, int whence) {
    auto* b = (Bytes*)from;
    int64_t at = whence == SEEK_SET ? offset : whence == SEEK_CUR ? (int64_t)b->at + offset : (int64_t)b->size + offset;
    if (at < 0 || at > (int64_t)b->size) return -1;
    b->at = (size_t)at;
    return 0;
}
int __cdecl CloseBytes(void*) { return 0; }
long __cdecl TellBytes(void* from) { return (long)((Bytes*)from)->at; }

// The file as the game's decoder will find it. False with no reason: there is no such file (most scenes of most games)
bool Playable(const std::wstring& file, std::string& why) {
    WIN32_FILE_ATTRIBUTE_DATA about;
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &about) || (about.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    if (about.nFileSizeHigh || about.nFileSizeLow < 4096 || about.nFileSizeLow > (64u << 20)) return why = "its size", false;
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(file, bytes) || bytes.size() < 4096 || memcmp(bytes.data(), "OggS", 4) != 0) return why = "not an Ogg file", false;
    alignas(8) uint8_t vf[0x400] = {};  // OggVorbis_File: 0x2D0 bytes as the game allocates it
    Bytes from{bytes.data(), bytes.size(), 0};
    // ov_open_callbacks(datasource, vf, initial, ibytes, ov_callbacks by value: read, seek, close, tell)
    int err = Call<0x818074, int>((void*)&from, (void*)vf, (const char*)nullptr, 0L, &ReadBytes, &SeekBytes, &CloseBytes, &TellBytes);
    if (err != 0) return why = Format("not Ogg Vorbis (%d)", err), false;
    const int32_t* info = Call<0x81806E, const int32_t*>((void*)vf, -1);  // ov_info: vorbis_info {version, channels, rate}
    int channels = info ? info[1] : 0, rate = info ? info[2] : 0;
    double secs = Call<0x81807A, double>((void*)vf, -1);  // ov_time_total
    Call<0x818062, int>((void*)vf);                       // ov_clear
    if (channels < 1 || channels > 2 || rate < 8000 || rate > 48000 || !(secs >= 1.0 && secs < 3600.0))
        return why = Format("%d channels, %d Hz, %.1f s", channels, rate, secs), false;
    return true;
}

// The cutscene that loads now asks for its track: whose voice it gets
void Choose(int track) {
    std::string scene = SceneName(), why;
    int model = scene.empty() ? 0 : SceneCjModel();
    char path[MAX_PATH] = {};
    if (model) {
        std::wstring file = Cfg().coopDir + L"voices\\" + (SkinIsFemale(model) ? L"female\\" : L"male\\") + Utf8ToWide(scene) + L".ogg";
        BOOL lossy = FALSE;  // the game opens it with CreateFileA
        if (!Playable(file, why)) {
            if (!why.empty()) LOGW("Cutscene %s: the skin's voice file is left alone (%s)", scene.c_str(), why.c_str());
        } else if (!WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, file.c_str(), -1, path, MAX_PATH, nullptr, &lossy) || lossy) {
            LOGW("Cutscene %s: the path of the skin's voice file is not one the game can open", scene.c_str());
            path[0] = 0;
        }
    }
    g_opened = false;  // the track is asked for after this, so the stream thread is not at it now
    g_scene = scene;
    std::lock_guard<std::mutex> hold(g_lock);
    g_track = path[0] ? track : -1;
    memcpy(g_path, path, sizeof(g_path));
}

using PreloadFn = void(__fastcall*)(void*, void*, int, int);  // CAudioEngine::PreloadCutsceneTrack(int16 track, bool wait)
PreloadFn g_origPreload = nullptr;
void __fastcall HookPreload(void* audio, void* edx, int track, int wait) {
    Choose((int16_t)track);
    g_origPreload(audio, edx, track, wait);
    bool chosen;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        chosen = g_track >= 0;
        if (wait) g_track = -1;  // the track is ready, its stream made: the file is given for this request alone
    }
    if (g_opened) LOGI("Cutscene %s: the skin's voice plays (model %d)", g_scene.c_str(), SceneCjModel());
    else if (chosen) LOGW("Cutscene %s: the game did not open the skin's voice file", g_scene.c_str());
}

using GetStreamFn = void*(__fastcall*)(void*, void*, uint32_t);
GetStreamFn g_origGetStream = nullptr;
void* __fastcall HookGetDataStream(void* loader, void* edx, uint32_t track) {  // the stream thread
    char path[MAX_PATH];
    path[0] = 0;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_track >= 0 && (uint32_t)g_track == track) memcpy(path, g_path, sizeof(path));
    }
    if (path[0]) {
        // as the game makes its own (0x4E0DA2-0x4E0E66): the name and the object from its operator new, the current
        // folder set to the game's, the stream opened; one that would not open is deleted and the game's own is made
        size_t size = strnlen(path, MAX_PATH - 1) + 1;
        char* name = Call<0x82119A, char*>((uint32_t)size);
        void* stream = name ? Call<0x82119A, void*>(0x28u) : nullptr;
        if (stream) {
            memcpy(name, path, size);
            name[size - 1] = 0;
            // CAEDataStream(track, name, start, length, encrypted): length 0 is the file's own (Initialise)
            CallMethod<0x4DC620, void*>(stream, (int32_t)track, name, 0u, 0u, 0);
            Call<0x5387D0>((const char*)0x858B54);  // CFileMgr::SetDir("")
            if (CallMethod<0x4DC2B0, bool>(stream)) {
                g_opened = true;
                return stream;
            }
            CallMethod<0x4DC490>(stream);  // closes it, deletes the name
            Call<0x8214BD>(stream);        // operator delete
        } else if (name) {
            Call<0x8214BD>((void*)name);
        }
    }
    return g_origGetStream(loader, edx, track);
}

class SceneVoice : public Module {
public:
    const char* Name() const override { return "SceneVoice"; }

    void OnInstallHooks() override {
        mem::HookFn(0x4E0D20, &HookGetDataStream, &g_origGetStream, "CAETrackLoader::GetDataStream");
        mem::RedirectCallFn(0x5B05FC, &HookPreload, &g_origPreload);  // CCutsceneMgr::LoadCutsceneData_preload
    }
};
SceneVoice g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;

}  // namespace

bool SkinIsFemale(int model) {
    void* info = ModelInfoOf(model);
    int type = info ? Field<int32_t>(info, 0x28) : 4;  // CPedModelInfo::m_nPedType
    return type == 5 || type == 22;                     // CIVFEMALE, PROSTITUTE
}

int SceneCjModel() {
    // MODEL_CSPLAY plays the story's CJ: in the host's cutscene on a partner's screen that is the host, whose copy here
    // wears its skin. Our own player's there showed the partner twice and the host nowhere (10.11); with no skin on the
    // host CSPLAY stays, dressed in the host's clothes and body (Missions' DressCutsceneCj)
    CPed* cj = FindPlayerPed(0);
    if (Net().Online() && HostCutsceneHere()) {
        const RemotePlayer* host = Players::Instance().Get(Net().HostId());
        cj = host ? host->ped : nullptr;
    }
    int skin = cj ? cj->ModelIndex() : 0;
    return skin != 0 && IsPedModel(skin) && IsModelLoaded(skin) ? skin : 0;
}

bool SceneVoiceOn() { return g_opened && Ref<uint8_t>(0xB5F851) && !g_scene.empty() && g_scene == SceneName(); }  // ms_running

bool SceneLine(const char* text, std::string& shown) {
    if (!text || !SceneVoiceOn()) return false;
    std::string line(text, strnlen(text, 399));  // the lines' pool holds 399
    shown = WithoutName(line);
    return shown != line;
}

}  // namespace coop::skin
