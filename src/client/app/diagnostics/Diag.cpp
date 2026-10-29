// Startup diagnostics (enabled with [debug] verbose_log = 1): logs skeleton events and the result
// of the critical initialisation steps, which makes "the game silently quits" problems traceable.
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "app/crash-handler/CrashHandler.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using EventFn = int(__cdecl*)(int, void*);
EventFn g_origEvent = nullptr;
int g_eventLogBudget = 400;
int g_lastState = -1;

int __cdecl HookRsEventHandler(int ev, void* param) {
    Heartbeat();
    int st = game::GameState();
    if (st != g_lastState) {
        LOGD("gGameState %d -> %d", g_lastState, st);
        g_lastState = st;
    }
    int r = g_origEvent(ev, param);
    // rsIDLE (26) and rsFRONTENDIDLE (27) fire every frame; log only the rare events
    if (g_eventLogBudget > 0 && ev != 26 && ev != 27) {
        --g_eventLogBudget;
        LOGD("RsEventHandler(%d) -> %d (quit=%d)", ev, r, game::Ref<uint8_t>(0xC17050));
    }
    return r;
}

using BoolFn = bool(__cdecl*)();
BoolFn g_origEssentials = nullptr;
bool __cdecl HookInitialiseEssentialsAfterRW() {
    bool r = g_origEssentials();
    LOGI("CGame::InitialiseEssentialsAfterRW -> %d", r);
    return r;
}

// The game runs 96 scripts at most (CTheScripts::ScriptsArray): a new one with every slot taken crashed the partner's game
// at the end of a host's cutscene (CTheScripts::StartNewScript read the empty free list, #15). Nearly full, the log
// tells which ones run, once each time it gets there.
uint64_t g_scriptsCheckMs = 0;
bool g_scriptsFull = false;
void CheckScripts() {
    if (NowMs() - g_scriptsCheckMs < 2000) return;
    g_scriptsCheckMs = NowMs();
    int n = 0;
    std::string names;
    for (void* s = game::Ref<void*>(0xA8B42C); s && n < 200; s = game::Field<void*>(s, 0x00)) {  // CTheScripts::pActiveScripts
        ++n;
        names += std::string(game::Field<char[8]>(s, 0x08), strnlen(game::Field<char[8]>(s, 0x08), 8)) + ' ';
    }
    bool full = n > 85;
    if (full && !g_scriptsFull) LOGW("%d of 96 scripts run: %s", n, names.c_str());
    g_scriptsFull = full;
}

// A cutscene starts only when every one of its models is in memory (CCutsceneMgr::LoadCutsceneData_loading looks each
// frame and waits). A model that never comes holds the mission on its black screen for good while the game runs (27.10:
// a host's cutscenes stayed black, and its pause map and the range's marker, which wait for models too, never came).
// After 10 s the log names the missing models and what the streamer does.
uint64_t g_csLoadSince = 0;
bool g_csLoadTold = false;
void CheckCutsceneLoad() {
    if (game::Ref<int32_t>(0xB5F84C) != 1) {  // CCutsceneMgr::ms_cutsceneLoadStatus: LOADING
        g_csLoadSince = 0;
        g_csLoadTold = false;
        return;
    }
    if (!g_csLoadSince) g_csLoadSince = NowMs();
    if (g_csLoadTold || NowMs() - g_csLoadSince < 10000) return;
    g_csLoadTold = true;
    std::string missing;
    int n = std::clamp(game::Ref<int32_t>(0xBC3FE8), 0, 50);  // ms_numLoadObjectNames
    for (int i = 0; i < n; ++i) {
        int model = game::Ref<int32_t>(0xBC31C0 + (uintptr_t)i * 4);  // ms_iModelIndex
        if (!game::IsModelLoaded(model))
            missing += Format("%.31s %d (state %d), ", (const char*)(0xBC38C8 + (uintptr_t)i * 32), model, game::ModelLoadState(model));
    }
    // CStreaming: ms_memoryUsedBytes, ms_memoryAvailable, ms_numModelsRequested, ms_numPriorityRequests, both channels'
    // states (ms_channel 0x98 bytes each, +0x80) and ms_disableStreaming
    LOGW("Cutscene %.8s has waited 10 s for its models: %sstreaming %u of %u KB, %d asked for (%u first), channels %d %d%s",
         (const char*)0xBC3F0C, missing.empty() ? "all in memory, " : missing.c_str(), game::Ref<uint32_t>(0x8E4CB4) / 1024,
         game::Ref<uint32_t>(0x8A5A80) / 1024, game::Ref<int32_t>(0x8E4CB8), game::Ref<uint32_t>(0x8E4BA0),
         game::Ref<int32_t>(0x8E4A60 + 0x80), game::Ref<int32_t>(0x8E4A60 + 0x98 + 0x80), game::Ref<uint8_t>(0x9654B0) ? ", streaming off" : "");
}

class DiagModule : public Module {
public:
    const char* Name() const override { return "Diagnostics"; }
    void OnInstallHooks() override {
        if (!Cfg().verboseLog) return;
        mem::HookFn(0x619B60, &HookRsEventHandler, &g_origEvent, "RsEventHandler");
        mem::HookFn(0x5BA160, &HookInitialiseEssentialsAfterRW, &g_origEssentials, "CGame::InitialiseEssentialsAfterRW");
    }
    void OnPostProcess() override {
        if (!core::IsPlayerReady()) return;
        CheckScripts();
        CheckCutsceneLoad();
    }
};

DiagModule g_diag;
struct Reg {
    Reg() { core::RegisterModule(&g_diag); }
} g_reg;
}  // namespace

void LogShutdownState() {
    LOGI("Process detach: gGameState=%d RsGlobal.quit=%d", game::GameState(), game::Ref<uint8_t>(0xC17050));
}

}  // namespace coop
