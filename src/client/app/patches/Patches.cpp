#include "app/patches/Patches.h"

#include <d3d9.h>
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstring>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/input/Input.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop::patches {

namespace {

// ---------------------------------------------------------------- mouse / focus
using MenuFn = void(__fastcall*)(void*, void*);
MenuFn g_origCentreMouse = nullptr;
void __fastcall HookCentreMousePointer(void* self, void* edx) {
    // Running in background: never steal the OS cursor from other applications.
    if (!game::IsForeground() || ui::In().MouseCaptured()) return;
    g_origCentreMouse(self, edx);
}

using MouseSetPosFn = void(__cdecl*)(void*);
MouseSetPosFn g_origIdleMouseSetPos = nullptr;
void __cdecl HookIdleMouseSetPos(void* pos) {
    // Idle() puts the OS cursor in the middle of the window every frame in game: not while our menu uses the
    // mouse, and not in the background (it would drag the cursor away from other applications).
    if (!game::IsForeground() || ui::In().MouseCaptured()) return;
    g_origIdleMouseSetPos(pos);
}

using PadFn = void(__fastcall*)(void*, void*);
PadFn g_origUpdateMouse = nullptr;
void __fastcall HookUpdateMouse(void* self, void* edx) {
    if (ui::In().MouseRelative() && !ui::In().MouseCaptured()) {
        g_origUpdateMouse(self, edx);  // the device is read: nothing is left there for the camera later
        // CMouseControllerState: X at +0x0C, Y at +0x10, DirectInput's lX and lY times -1 where the controls menu
        // inverts them (CMousePointerStateHelper at 0xBA6744: horizontally, 0xBA6745: vertically; not while the menu
        // is open, 0xBA67A4): see 0x53F427 in CPad::UpdateMouse. Taken back to DirectInput's own, y down.
        float x = game::Ref<float>(0xB73418 + 0x0C), y = game::Ref<float>(0xB73418 + 0x10);
        if (!game::Ref<uint8_t>(0xBA67A4)) {
            if (game::Ref<uint8_t>(0xBA6744)) x = -x;
            if (game::Ref<uint8_t>(0xBA6745)) y = -y;
        }
        ui::In().AddMouseMotion(x, y);
        memset((void*)0xB73418, 0, 0x14);  // CPad::NewMouseControllerState: no camera turn, no shot
        memset((void*)0xB7342C, 0, 0x14);  // CPad::OldMouseControllerState
        return;
    }
    if (ui::In().MouseCaptured()) {
        memset((void*)0xB73418, 0, 0x14);  // CPad::NewMouseControllerState
        memset((void*)0xB7342C, 0, 0x14);  // CPad::OldMouseControllerState
        return;
    }
    g_origUpdateMouse(self, edx);
}

// ---------------------------------------------------------------- pause
// Audio asks IsForegroundApp() (0x746060): the stream thread (CAEStreamThread::MainLoop) pauses when the window
// is not focused. In background mode that would freeze loading screens (the loading tune never finishes,
// CAudioEngine::InitialisePostLoading waits for it forever). CAEAudioHardware::Service asks too, but throws the
// answer away (eax is overwritten at 0x4D987A): its redirect changes nothing.
bool __cdecl AudioIsForegroundApp() { return true; }

// WinMain runs a frame (RsEventHandler(rsIDLE, TRUE) -> Idle) only while the window is in the foreground. Running in the
// background it drew on, and in exclusive fullscreen an alt-tab loses the D3D device: what was drawn then left the
// screen corrupted after the return and the game crashed in d3d9 later. While the device is lost a frame is processed
// without drawing (Idle with no argument returns before the rendering), and RenderWare resets the device on the first
// drawn frame as it does after the game's own pause.
using RsEventFn = int(__cdecl*)(int, void*);
RsEventFn g_origIdleEvent = nullptr;
int __cdecl HookIdleEvent(int event, void* param) {
    auto* dev = (IDirect3DDevice9*)game::D3DDevice();
    if (dev && dev->TestCooperativeLevel() == D3DERR_DEVICELOST) param = nullptr;
    return g_origIdleEvent(event, param);
}

using VoidFn = void(__cdecl*)();
VoidFn g_origStartUserPause = nullptr;
void __cdecl HookStartUserPause() {
    // In a co-op session the world is shared: the pause menu must not freeze it.
    if (Cfg().noPauseMenu && core::IsInCoopSession()) return;
    g_origStartUserPause();
}

// The menu stops the radio as it closes and as it leaves the audio page, counting on the pause: under it
// CAERadioTrackManager::StopRadio keeps the car's radio playing. Unpaused it stopped it, and the radio stayed silent
// until the player got in again. With the car's radio on the stop is skipped; on foot it still ends the page's preview.
using StopRadioFn = void(__fastcall*)(void*, void*, void*, bool);
StopRadioFn g_origMenuStopRadio = nullptr;
void __fastcall HookMenuStopRadio(void* audio, void* edx, void* settings, bool duringPause) {
    if (Cfg().noPauseMenu && core::IsInCoopSession() && game::Call<0x4E9800, bool>()) return;  // IsVehicleRadioActive
    g_origMenuStopRadio(audio, edx, settings, duringPause);
}

// ---------------------------------------------------------------- video mode / windowed / intro
struct RwVideoMode {
    int width, height, depth, flags, refRate, format;
};
constexpr int rwVIDEOMODEEXCLUSIVE = 1;

void AdjustWindow(HWND hwnd, int w, int h) {
    auto& cfg = Cfg();
    LONG style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    SetWindowLongW(hwnd, GWL_STYLE, style);
    SetWindowLongW(hwnd, GWL_EXSTYLE, 0);
    RECT rc{0, 0, (LONG)w, (LONG)h};
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = cfg.windowX, y = cfg.windowY;
    if (x < 0 || y < 0) {
        x = work.left + ((work.right - work.left) - ww) / 2;
        y = work.top + ((work.bottom - work.top) - wh) / 2;
    }
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
    SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

// Picks the largest size with the requested aspect that fits into the work area together with the frame.
void FitToWorkArea(int& w, int& h) {
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT frame{0, 0, 100, 100};
    AdjustWindowRectEx(&frame, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    int maxW = (work.right - work.left) - ((frame.right - frame.left) - 100);
    int maxH = (work.bottom - work.top) - ((frame.bottom - frame.top) - 100);
    if (w <= maxW && h <= maxH) return;
    float k = std::min((float)maxW / (float)w, (float)maxH / (float)h);
    w = (int)(w * k) & ~1;
    h = (int)(h * k) & ~1;
}

bool g_windowedActive = false;

// SetVideoMode (0x745C70) is called by CMenuManager::Initialise with the resolution saved in
// gta_sa.set and by the resolution option; it would switch RW back to an exclusive mode.
using SetVideoModeFn = void(__cdecl*)(int);
SetVideoModeFn g_origSetVideoMode = nullptr;
void __cdecl HookSetVideoMode(int mode) {
    if (g_windowedActive) {
        LOGI("SetVideoMode(%d) ignored in windowed mode", mode);
        return;
    }
    g_origSetVideoMode(mode);
}

using SelectDeviceFn = int(__cdecl*)();
SelectDeviceFn g_origSelectDevice = nullptr;
int __cdecl HookSelectDevice() {
    auto& cfg = Cfg();
    if (cfg.width > 0 && cfg.height > 0 && !cfg.windowed) {
        int n = game::Call<0x7F2CC0, int>();  // RwEngineGetNumVideoModes
        for (int i = 1; i < n; ++i) {
            RwVideoMode vm{};
            game::Call<0x7F2CF0, void*>(&vm, i);  // RwEngineGetVideoModeInfo
            if (vm.width == cfg.width && vm.height == cfg.height && vm.depth == 32) {
                game::Ref<int32_t>(game::addr::MenuManager + 0xD8) = i;  // m_nDisplayVideoMode
                game::Ref<int32_t>(game::addr::MenuManager + 0xD4) = i;  // m_nPrefsVideoMode
                LOGI("Video mode forced to #%d (%dx%d)", i, cfg.width, cfg.height);
                break;
            }
        }
    }
    int r = g_origSelectDevice();
    if (r && cfg.windowed) {
        // RenderWare's video mode 0 is the native windowed mode of the D3D9 driver: switching to it
        // before RwEngineStart makes RW create a windowed device by itself.
        RwVideoMode vm0{};
        game::Call<0x7F2CF0, void*>(&vm0, 0);
        if (!(vm0.flags & rwVIDEOMODEEXCLUSIVE)) {
            // mode 0 reports the "desktop" size, which GetAdapterDisplayMode (hooked) replaced
            // with the requested window size
            int w = vm0.width, h = vm0.height;
            if (game::Call<0x7F2D50, int>(0)) {  // RwEngineSetVideoMode(0)
                game::Ref<int32_t>(0xC17044) = w;  // RsGlobal.maximumWidth
                game::Ref<int32_t>(0xC17048) = h;  // RsGlobal.maximumHeight
                void* ps = game::Ref<void*>(0xC17054);
                if (ps) game::Field<int32_t>(ps, 8) = 0;  // psGlobal.fullScreen = FALSE
                if (HWND hwnd = (HWND)game::GameWindow()) AdjustWindow(hwnd, w, h);
                LOGI("Windowed mode %dx%d", w, h);
                g_windowedActive = true;
            } else {
                LOGW("RwEngineSetVideoMode(0) failed, staying fullscreen");
            }
        } else {
            LOGW("RW video mode 0 is exclusive, windowed mode unavailable");
        }
        if (!g_windowedActive && cfg.windowedStrict) {
            LOGE("windowed_strict: windowed mode could not be applied, quitting before the screen is taken");
            TerminateProcess(GetCurrentProcess(), 3);
        }
    }
    if (cfg.skipIntro && game::GameState() == game::GAME_STATE_INITIAL) {
        game::Ref<int32_t>(game::addr::GameState) = game::GAME_STATE_FRONTEND_LOADING;
    }
    return r;
}

// ---------------------------------------------------------------- windowed: fake desktop size
// RenderWare's windowed video mode (#0) uses the desktop resolution reported by
// IDirect3D9::GetAdapterDisplayMode. Reporting the requested window size instead makes RW create a
// windowed device of exactly that size with all internal structures consistent.
int g_winW = 0, g_winH = 0;
using GetAdapterDisplayModeFn = HRESULT(WINAPI*)(IDirect3D9*, UINT, D3DDISPLAYMODE*);
using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);
GetAdapterDisplayModeFn g_origGetAdapterDisplayMode = nullptr;
Direct3DCreate9Fn g_origD3DCreate = nullptr;

HRESULT WINAPI HookGetAdapterDisplayMode(IDirect3D9* d3d, UINT adapter, D3DDISPLAYMODE* mode) {
    HRESULT hr = g_origGetAdapterDisplayMode(d3d, adapter, mode);
    if (SUCCEEDED(hr) && mode && g_winW > 0) {
        mode->Width = (UINT)g_winW;
        mode->Height = (UINT)g_winH;
    }
    return hr;
}

IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdk) {
    IDirect3D9* d3d = g_origD3DCreate(sdk);
    if (d3d && !g_origGetAdapterDisplayMode) {
        void** vt = *reinterpret_cast<void***>(d3d);
        g_origGetAdapterDisplayMode =
            (GetAdapterDisplayModeFn)mem::SwapVTable(vt, 8, (void*)&HookGetAdapterDisplayMode);
    }
    return d3d;
}

void InstallWindowedMode() {
    auto& cfg = Cfg();
    g_winW = cfg.width > 0 ? cfg.width : 1280;
    g_winH = cfg.height > 0 ? cfg.height : 720;
    FitToWorkArea(g_winW, g_winH);
    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9) d3d9 = LoadLibraryW(L"d3d9.dll");
    void* fn = d3d9 ? (void*)GetProcAddress(d3d9, "Direct3DCreate9") : nullptr;
    if (!fn) {
        LOGW("Windowed mode: Direct3DCreate9 not found");
        return;
    }
    mem::Hook((uintptr_t)fn, (void*)&HookDirect3DCreate9, (void**)&g_origD3DCreate, "Direct3DCreate9");
    LOGI("Windowed mode requested: %dx%d", g_winW, g_winH);
}

// ---------------------------------------------------------------- user files directory
// The game builds "<Documents>\GTA San Andreas User Files". Several mods (SilentPatch, RepairGTA)
// re-implement InitUserDirectories, so instead of hooking it we make the whole process see another
// "Documents" folder: SHGetFolderPathA/W, SHGetKnownFolderPath and the "Personal" shell folder value.
std::wstring g_docsW;  // fake Documents folder (contains "GTA San Andreas User Files")
std::string g_docsA;

using SHGetFolderPathWFn = HRESULT(WINAPI*)(HWND, int, HANDLE, DWORD, LPWSTR);
using SHGetFolderPathAFn = HRESULT(WINAPI*)(HWND, int, HANDLE, DWORD, LPSTR);
using SHGetKnownFolderPathFn = HRESULT(WINAPI*)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR*);
using RegQueryValueExAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using RegQueryValueExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
SHGetFolderPathWFn g_origSHGetFolderPathW = nullptr;
SHGetFolderPathAFn g_origSHGetFolderPathA = nullptr;
SHGetKnownFolderPathFn g_origSHGetKnownFolderPath = nullptr;
RegQueryValueExAFn g_origRegQueryValueExA = nullptr;
RegQueryValueExWFn g_origRegQueryValueExW = nullptr;

HRESULT WINAPI HookSHGetFolderPathW(HWND h, int csidl, HANDLE t, DWORD f, LPWSTR path) {
    if ((csidl & 0xFF) == CSIDL_PERSONAL && path) {
        wcsncpy(path, g_docsW.c_str(), MAX_PATH - 1);
        path[MAX_PATH - 1] = 0;
        return S_OK;
    }
    return g_origSHGetFolderPathW(h, csidl, t, f, path);
}

HRESULT WINAPI HookSHGetFolderPathA(HWND h, int csidl, HANDLE t, DWORD f, LPSTR path) {
    if ((csidl & 0xFF) == CSIDL_PERSONAL && path) {
        strncpy(path, g_docsA.c_str(), MAX_PATH - 1);
        path[MAX_PATH - 1] = 0;
        return S_OK;
    }
    return g_origSHGetFolderPathA(h, csidl, t, f, path);
}

HRESULT WINAPI HookSHGetKnownFolderPath(REFKNOWNFOLDERID id, DWORD f, HANDLE t, PWSTR* out) {
    if (out && IsEqualGUID(id, FOLDERID_Documents)) {
        size_t bytes = (g_docsW.size() + 1) * sizeof(wchar_t);
        auto* p = (PWSTR)CoTaskMemAlloc(bytes);
        if (!p) return E_OUTOFMEMORY;
        memcpy(p, g_docsW.c_str(), bytes);
        *out = p;
        return S_OK;
    }
    return g_origSHGetKnownFolderPath(id, f, t, out);
}

LSTATUS WINAPI HookRegQueryValueExA(HKEY k, LPCSTR name, LPDWORD r, LPDWORD type, LPBYTE data, LPDWORD size) {
    if (name && _stricmp(name, "Personal") == 0 && size) {
        DWORD need = (DWORD)g_docsA.size() + 1;
        if (type) *type = REG_SZ;
        if (!data) {
            *size = need;
            return ERROR_SUCCESS;
        }
        if (*size < need) {
            *size = need;
            return ERROR_MORE_DATA;
        }
        memcpy(data, g_docsA.c_str(), need);
        *size = need;
        return ERROR_SUCCESS;
    }
    return g_origRegQueryValueExA(k, name, r, type, data, size);
}

LSTATUS WINAPI HookRegQueryValueExW(HKEY k, LPCWSTR name, LPDWORD r, LPDWORD type, LPBYTE data, LPDWORD size) {
    if (name && _wcsicmp(name, L"Personal") == 0 && size) {
        DWORD need = (DWORD)((g_docsW.size() + 1) * sizeof(wchar_t));
        if (type) *type = REG_SZ;
        if (!data) {
            *size = need;
            return ERROR_SUCCESS;
        }
        if (*size < need) {
            *size = need;
            return ERROR_MORE_DATA;
        }
        memcpy(data, g_docsW.c_str(), need);
        *size = need;
        return ERROR_SUCCESS;
    }
    return g_origRegQueryValueExW(k, name, r, type, data, size);
}

std::string ToAnsi(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 1, 'X');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    s.resize(strlen(s.c_str()));
    return s;
}

template <typename F>
void HookExport(const wchar_t* dll, const char* name, F detour, F* orig) {
    HMODULE m = GetModuleHandleW(dll);
    if (!m) m = LoadLibraryW(dll);
    void* fn = m ? (void*)GetProcAddress(m, name) : nullptr;
    if (fn) mem::Hook((uintptr_t)fn, (void*)detour, (void**)orig, name);
}

void SetUserDir(const std::wstring& dirIn) {
    std::wstring base = dirIn;
    while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
    if (base.empty()) return;
    std::wstring userFiles = base + L"\\GTA San Andreas User Files";
    EnsureDirectory(userFiles);
    CreateDirectoryW((userFiles + L"\\Gallery").c_str(), nullptr);
    CreateDirectoryW((userFiles + L"\\User Tracks").c_str(), nullptr);
    // seed settings from the real profile (read before the hooks are installed)
    wchar_t docs[MAX_PATH];
    if (!FileExists(userFiles + L"\\gta_sa.set") && SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) {
        std::wstring src = std::wstring(docs) + L"\\GTA San Andreas User Files\\gta_sa.set";
        CopyFileW(src.c_str(), (userFiles + L"\\gta_sa.set").c_str(), TRUE);
    }
    g_docsW = base;
    g_docsA = ToAnsi(base);
    HookExport(L"shell32.dll", "SHGetFolderPathW", &HookSHGetFolderPathW, &g_origSHGetFolderPathW);
    HookExport(L"shell32.dll", "SHGetFolderPathA", &HookSHGetFolderPathA, &g_origSHGetFolderPathA);
    HookExport(L"shell32.dll", "SHGetKnownFolderPath", &HookSHGetKnownFolderPath, &g_origSHGetKnownFolderPath);
    HookExport(L"advapi32.dll", "RegQueryValueExA", &HookRegQueryValueExA, &g_origRegQueryValueExA);
    HookExport(L"advapi32.dll", "RegQueryValueExW", &HookRegQueryValueExW, &g_origRegQueryValueExW);
    LOGI("Documents folder redirected to: %s", WideToUtf8(base).c_str());
}

// ---------------------------------------------------------------- multi-instance isolation
// modloader shares state through a named file mapping; a second game copy would open the first
// copy's mapping (with pointers valid only there) and crash. With multi_instance enabled every named
// mapping gets a per-process suffix.
using CreateFileMappingAFn = HANDLE(WINAPI*)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR);
using CreateFileMappingWFn = HANDLE(WINAPI*)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
using OpenFileMappingAFn = HANDLE(WINAPI*)(DWORD, BOOL, LPCSTR);
using OpenFileMappingWFn = HANDLE(WINAPI*)(DWORD, BOOL, LPCWSTR);
CreateFileMappingAFn g_origCreateFileMappingA = nullptr;
CreateFileMappingWFn g_origCreateFileMappingW = nullptr;
OpenFileMappingAFn g_origOpenFileMappingA = nullptr;
OpenFileMappingWFn g_origOpenFileMappingW = nullptr;

std::string UniqueA(LPCSTR name) { return std::string(name) + "#sacoop" + std::to_string(GetCurrentProcessId()); }
std::wstring UniqueW(LPCWSTR name) { return std::wstring(name) + L"#sacoop" + std::to_wstring(GetCurrentProcessId()); }

HANDLE WINAPI HookCreateFileMappingA(HANDLE f, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCSTR name) {
    if (!name) return g_origCreateFileMappingA(f, sa, prot, hi, lo, name);
    return g_origCreateFileMappingA(f, sa, prot, hi, lo, UniqueA(name).c_str());
}
HANDLE WINAPI HookCreateFileMappingW(HANDLE f, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCWSTR name) {
    if (!name) return g_origCreateFileMappingW(f, sa, prot, hi, lo, name);
    return g_origCreateFileMappingW(f, sa, prot, hi, lo, UniqueW(name).c_str());
}
HANDLE WINAPI HookOpenFileMappingA(DWORD access, BOOL inherit, LPCSTR name) {
    if (!name) return g_origOpenFileMappingA(access, inherit, name);
    return g_origOpenFileMappingA(access, inherit, UniqueA(name).c_str());
}
HANDLE WINAPI HookOpenFileMappingW(DWORD access, BOOL inherit, LPCWSTR name) {
    if (!name) return g_origOpenFileMappingW(access, inherit, name);
    return g_origOpenFileMappingW(access, inherit, UniqueW(name).c_str());
}

using CreateSemaphoreAFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LONG, LONG, LPCSTR);
using CreateEventAFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCSTR);
using CreateMutexAFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
CreateSemaphoreAFn g_origCreateSemaphoreA = nullptr;
CreateEventAFn g_origCreateEventA = nullptr;
CreateMutexAFn g_origCreateMutexA = nullptr;

// gta_sa.exe creates the streaming semaphore with the global name "CdStream": two game copies would
// share one kernel object and steal each other's read requests (the second copy hangs while loading).
HANDLE WINAPI HookCreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG init, LONG max, LPCSTR name) {
    if (!name) return g_origCreateSemaphoreA(sa, init, max, name);
    return g_origCreateSemaphoreA(sa, init, max, UniqueA(name).c_str());
}
HANDLE WINAPI HookCreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name) {
    if (!name) return g_origCreateEventA(sa, manual, initial, name);
    return g_origCreateEventA(sa, manual, initial, UniqueA(name).c_str());
}
HANDLE WINAPI HookCreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCSTR name) {
    if (!name) return g_origCreateMutexA(sa, owner, name);
    return g_origCreateMutexA(sa, owner, UniqueA(name).c_str());
}

void IsolateInstance() {
    HookExport(L"kernel32.dll", "CreateSemaphoreA", &HookCreateSemaphoreA, &g_origCreateSemaphoreA);
    HookExport(L"kernel32.dll", "CreateEventA", &HookCreateEventA, &g_origCreateEventA);
    HookExport(L"kernel32.dll", "CreateMutexA", &HookCreateMutexA, &g_origCreateMutexA);
    HookExport(L"kernel32.dll", "CreateFileMappingA", &HookCreateFileMappingA, &g_origCreateFileMappingA);
    HookExport(L"kernel32.dll", "CreateFileMappingW", &HookCreateFileMappingW, &g_origCreateFileMappingW);
    HookExport(L"kernel32.dll", "OpenFileMappingA", &HookOpenFileMappingA, &g_origOpenFileMappingA);
    HookExport(L"kernel32.dll", "OpenFileMappingW", &HookOpenFileMappingW, &g_origOpenFileMappingW);
    LOGI("Multi-instance isolation enabled (named kernel objects are per process)");
}

}  // namespace

// Plugins built with an old "injector" (MixSets, Peds Extender, CLEO+ and others) save the registers for an inline hook
// with "add byte ptr [esp+0Ch], 4". When the stack pointer ends in FC the carry is lost, so a hook that edits its return
// address writes it 256 bytes off and returns into the middle of an instruction (Peds Extender at 6133DB while a train
// gets its driver). Our hooks make the stack deeper, which moves that into places it never was: the add and the sub
// become full-width (the same length, one byte each).
void FixPluginWrappers() {
    static const uint8_t kWrapper[] = {0x60, 0x80, 0x44, 0x24, 0x0C, 0x04, 0x9C, 0x54, 0xE8, 0, 0, 0, 0,  // call: any
                                       0x83, 0xC4, 0x04, 0x80, 0x6C, 0x24, 0x10, 0x04, 0x9D, 0x61, 0xC3};
    std::wstring dir = ModuleDir();  // the game's folder: plugins live under it
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    int fixed = 0;
    std::string where;
    MODULEENTRY32W me{};
    me.dwSize = sizeof me;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        if (me.hModule == GetModuleHandleW(nullptr) || _wcsnicmp(me.szExePath, dir.c_str(), dir.size()) != 0) continue;
        int before = fixed;
        uint8_t* base = me.modBaseAddr;
        auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
            if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            uint8_t* p = base + sec[s].VirtualAddress;
            for (size_t i = 0; i + sizeof kWrapper <= sec[s].Misc.VirtualSize; ++i) {
                size_t k = 0;
                while (k < sizeof kWrapper && (p[i + k] == kWrapper[k] || (k >= 9 && k <= 12))) ++k;
                if (k < sizeof kWrapper) continue;
                mem::Put<uint8_t>((uintptr_t)(p + i + 1), 0x83);   // add dword ptr [esp+0Ch], 4
                mem::Put<uint8_t>((uintptr_t)(p + i + 16), 0x83);  // sub dword ptr [esp+10h], 4
                ++fixed;
            }
        }
        if (fixed > before) where += Format(" %s:%d", WideToUtf8(me.szModule).c_str(), fixed - before);
    }
    CloseHandle(snap);
    if (fixed) LOGI("Fixed %d register wrappers of old plugins (lost carry in the saved stack pointer):%s", fixed, where.c_str());
}

namespace {
// ---------------------------------------------------------------- engine guards
// Stencil shadows (high shadow quality) of map objects: CStencilShadows::RenderForObject checks only the triangle count
// of the object's shadow mesh, and CCollisionData::RemoveCollisionVolumes (its col file streamed out) frees the mesh
// but keeps the counts. An object still on the shadow list then crashed the game at 0x40F64C, right after rocket
// blasts in two players' reports. The shadow keeps the triangle count of the mesh it was made for (+4), and later the
// mesh pointers were not even null but small offsets (0x3C0): the col data had been loaded anew. A shadow is drawn
// only from the mesh it was made for, inside its col data's own block (CFileLoader::LoadCollisionModelVer3 allocates
// one for the data and its arrays).
using StencilForObjectFn = bool(__cdecl*)(void*, void*);
StencilForObjectFn g_origStencilForObject = nullptr;
bool __cdecl HookStencilForObject(void* shadow, void* arg) {
    auto* owner = *(game::CEntity**)shadow;                                  // CStencilShadowObject::m_pOwner
    void* col = owner ? game::CallMethod<0x535300, void*>(owner) : nullptr;  // CEntity::GetColModel
    auto* data = col ? game::Field<uint8_t*>(col, 0x2C) : nullptr;           // CColModel::m_pColData
    if (data) {
        auto* verts = game::Field<uint8_t*>(data, 0x28);  // CCollisionData::m_pShadowVertices
        auto* tris = game::Field<uint8_t*>(data, 0x2C);   // m_pShadowTriangles
        bool inside = verts > data && tris > data && verts - data < 0x100000 && tris - data < 0x100000;
        if (!inside || game::Field<uint32_t>(data, 0x20) != game::Field<uint16_t>(shadow, 4)) return false;  // m_nNumShadowTriangles
    }
    return g_origStencilForObject(shadow, arg);
}

// CAEStreamThread::Service starts a track at the asked position modulo the track's length (0x4F1464): a track whose
// decoder opens but reports no length (a broken or emptied file of a game's audio) divided by zero in the audio
// thread, in both players' games at once (Ryder's barber shop). Such a track starts from its beginning.
// Here: eax = the length (GetStreamLengthMs), [esp+0x1C] = the asked position; on to 0x4F1466 with the cursor in edx.
__attribute__((naked)) void StreamCursorStub() {
    __asm__ volatile(
        "movl %eax, %ecx\n\t"
        "movl 0x1c(%esp), %eax\n\t"
        "xorl %edx, %edx\n\t"
        "testl %ecx, %ecx\n\t"
        "jz 1f\n\t"
        "divl %ecx\n\t"
        "1:\n\t"
        "pushl $0x4F1466\n\t"
        "ret\n\t");
}

// A mission may create a car of a model it requested a while before without checking that it is still there: OG Loc's
// chase asks for the Manana while loading its car recordings, lets it go later and creates three of them on a timer.
// A model not in memory crashed CVehicleModelInfo::CreateInstance (0x4C9691) in a host's game. It is loaded first.
using CreateCarForScriptFn = game::CVehicle*(__cdecl*)(int, game::CVector, uint8_t);
CreateCarForScriptFn g_origCreateCarForScript = nullptr;
game::CVehicle* __cdecl HookCreateCarForScript(int model, game::CVector pos, uint8_t missionCleanup) {
    if (!game::IsModelLoaded(model) && game::IsVehicleModel(model)) {
        LOGW("A script creates a car of model %d that is not in memory: loading it first", model);
        game::RequestModel(model, 0x10 /* PRIORITY_REQUEST: the car holds it, nothing keeps it after */);
        game::LoadAllRequestedModels(false);
    }
    return g_origCreateCarForScript(model, pos, missionCleanup);
}

// The commands that make and move script entities (CREATE_CAR, CREATE_CHAR, CREATE_OBJECT, SET_CHAR_COORDINATES,
// WARP_CHAR_FROM_CAR_TO_COORD) clear the spot of this game's random people and cars that collide with the entity, with
// whoever sits in them. Ours make and move the copies of another game's people and things: a copy deleted this game's
// taxi with its driver when the other player, riding in it here, snapped to its place or came back, and a prop in a
// hand cleared the people next to it. Those clear nothing. A copy of a car still clears its spot (two cars in one
// place threw each other away), and this game's own player does as the game's scripts do.
using ClearSpaceFn = void(__cdecl*)(const game::CVector&, game::CEntity*);
ClearSpaceFn g_origClearSpace = nullptr;
void __cdecl HookClearSpace(const game::CVector& pos, game::CEntity* entity) {
    if (game::script::Running() && entity && !entity->IsVehicle() && entity != (game::CEntity*)game::FindPlayerPed(0)) return;
    g_origClearSpace(pos, entity);
}

}  // namespace

bool GameMouseEnabled() { return !ui::In().MouseCaptured(); }

void ApplyWindowPlacement() {
    if (!Cfg().windowed || g_winW <= 0) return;
    HWND hwnd = (HWND)game::GameWindow();
    if (!hwnd) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (Cfg().verboseLog) {
        static int dumps = 0;
        if (dumps++ < 12) {
            RECT wr;
            GetWindowRect(hwnd, &wr);
            LOGD("window: rect %ld,%ld %ldx%ld client %ldx%ld style %08lX RsGlobal %dx%d fullScreen %d",
                 wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, rc.right, rc.bottom, style,
                 game::ScreenWidth(), game::ScreenHeight(), game::Field<int32_t>(game::Ref<void*>(0xC17054), 8));
        }
    }
    if (Cfg().windowedStrict) {
        RwVideoMode cur{};
        int idx = game::Call<0x7F2D20, int>();  // RwEngineGetCurrentVideoMode
        game::Call<0x7F2CF0, void*>(&cur, idx);
        if (cur.flags & rwVIDEOMODEEXCLUSIVE) {
            LOGE("windowed_strict: the game switched to an exclusive (fullscreen) mode, quitting");
            TerminateProcess(GetCurrentProcess(), 3);
        }
    }
    if (rc.right == g_winW && rc.bottom == g_winH && (style & WS_CAPTION) == WS_CAPTION) return;
    static int attempts = 0;
    if (++attempts > 5) return;  // something keeps resizing the window: stop fighting it
    LOGI("Window placement: client %ldx%ld style %08lX -> %dx%d", rc.right, rc.bottom, style, g_winW, g_winH);
    AdjustWindow(hwnd, g_winW, g_winH);
}

void Apply() {
    auto& cfg = Cfg();

    // Native 2-player (split screen) mode must never activate: our remote player is a CPlayerPed
    // registered in CWorld::Players[1] only while it is processed. CGameLogic::IsCoopGameGoingOn -> false
    static const uint8_t retFalse[] = {0x33, 0xC0, 0xC3};  // xor eax,eax; ret
    mem::Write(0x441390, retFalse, sizeof(retFalse));

    if (cfg.multiInstance) {
        mem::Write(0x7468E0, retFalse, sizeof(retFalse));  // IsAlreadyRunning -> false
        IsolateInstance();
    }

    if (cfg.runInBackground) {
        // WinMain: "if (!ForegroundApp) { Sleep(100); continue; }" -> never sleep
        if (*(uint8_t*)0x748A8D == 0x0F && *(uint8_t*)0x748A8E == 0x84) mem::Nop(0x748A8D, 6);
        else LOGW("Background patch skipped: unexpected bytes at 0x748A8D (another mod?)");
        mem::RedirectCall(0x4D9875, (void*)&AudioIsForegroundApp, nullptr);  // CAEAudioHardware::Service
        mem::RedirectCall(0x4F15E1, (void*)&AudioIsForegroundApp, nullptr);  // CAEStreamThread::MainLoop
        mem::RedirectCallFn(0x748D9B, &HookIdleEvent, &g_origIdleEvent);    // WinMain -> RsEventHandler(rsIDLE)
    }
    mem::HookFn(0x57C520, &HookCentreMousePointer, &g_origCentreMouse, "CMenuManager::CentreMousePointer");
    mem::RedirectCallFn(0x53E9F1, &HookIdleMouseSetPos, &g_origIdleMouseSetPos);  // Idle -> RsMouseSetPos
    mem::HookFn(0x53F3C0, &HookUpdateMouse, &g_origUpdateMouse, "CPad::UpdateMouse");
    mem::HookFn(0x561AF0, &HookStartUserPause, &g_origStartUserPause, "CTimer::StartUserPause");
    mem::RedirectCallFn(0x576C55, &HookMenuStopRadio, &g_origMenuStopRadio);  // CMenuManager::CheckForMenuClosing
    mem::RedirectCallFn(0x5737A5, &HookMenuStopRadio, &g_origMenuStopRadio);  // CMenuManager::SwitchToNewScreen
    mem::HookFn(0x746190, &HookSelectDevice, &g_origSelectDevice, "psSelectDevice");
    mem::HookFn(0x710310, &HookStencilForObject, &g_origStencilForObject, "CStencilShadows::RenderForObject");
    mem::HookFn(0x431F80, &HookCreateCarForScript, &g_origCreateCarForScript, "CCarCtrl::CreateCarForScript");
    mem::HookFn(0x486B00, &HookClearSpace, &g_origClearSpace, "CTheScripts::ClearSpaceForMissionEntity");
    if (!memcmp((void*)0x4F145C, "\x8B\xC8\x8B\x44\x24\x1C\x33\xD2\xF7\xF1", 10)) {  // CAEStreamThread::Service
        mem::Jump(0x4F145C, (void*)&StreamCursorStub);
        mem::Nop(0x4F1461, 5);
    } else {
        LOGW("Stream cursor guard skipped: unexpected bytes at 0x4F145C (another mod?)");
    }
    if (cfg.windowed) {
        InstallWindowedMode();
        mem::HookFn(0x745C70, &HookSetVideoMode, &g_origSetVideoMode, "SetVideoMode");
    }


    std::wstring userDir = Utf8ToWide(cfg.userDir);
    if (!userDir.empty()) {
        if (userDir.size() > 1 && userDir[1] != L':' && userDir[0] != L'\\') userDir = cfg.coopDir + userDir;
        SetUserDir(userDir);
    }
}

}  // namespace coop::patches
