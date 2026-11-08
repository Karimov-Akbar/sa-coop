#include "app/crash-handler/CrashHandler.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>

#include "common/config/version/Version.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"

namespace coop {

namespace {
std::wstring g_crashPath;
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
volatile LONG g_firstChanceReports = 0, g_unhandledReports = 0;  // apart: first-chance ones used up the real crash's
volatile LONG g_inFilter = 0;
volatile LONG g_heartbeat = 0;
uintptr_t g_exeBase = 0, g_exeEnd = 0, g_selfBase = 0, g_selfEnd = 0;
DWORD g_mainThreadId = 0;
unsigned g_hangSeconds = 15;

bool DescribeAddress(uintptr_t addr, char* out, size_t outSize) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)addr, &mod) ||
        !mod) {
        snprintf(out, outSize, "%08X", (unsigned)addr);
        return false;
    }
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    std::string n = WideToUtf8(name);
    if (mod == GetModuleHandleW(nullptr))
        snprintf(out, outSize, "%08X (gta_sa.exe)", (unsigned)addr);
    else
        snprintf(out, outSize, "%08X (%s+%X)", (unsigned)addr, n.c_str(), (unsigned)(addr - (uintptr_t)mod));
    return true;
}

bool IsExecutable(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) return false;
    DWORD p = mbi.Protect & 0xFF;
    return mbi.State == MEM_COMMIT &&
           (p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY);
}

// Writes return-address-looking values of a stack (from its top at esp).
void WriteStack(FILE* f, const uint8_t* stack, size_t size) {
    int found = 0;
    for (size_t a = 0; a + 4 <= size && found < 40; a += 4) {
        uintptr_t v = *(const uintptr_t*)(stack + a);
        if (v < 0x401000 || !IsExecutable(v)) continue;
        char d[160];
        DescribeAddress(v, d, sizeof(d));
        fprintf(f, "  [esp+%03X] %s\r\n", (unsigned)a, d);
        ++found;
    }
}

void ModuleRange(HMODULE mod, uintptr_t& base, uintptr_t& end) {
    MODULEINFO mi{};
    if (mod && GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi)))
        base = (uintptr_t)mi.lpBaseOfDll, end = base + mi.SizeOfImage;
}

void WriteReport(EXCEPTION_POINTERS* ep, bool firstChance) {
    if (InterlockedIncrement(firstChance ? &g_firstChanceReports : &g_unhandledReports) > (firstChance ? 6 : 2)) return;
    FILE* f = _wfopen(g_crashPath.c_str(), L"ab");
    if (!f) return;
    auto* er = ep->ExceptionRecord;
    auto* ctx = ep->ContextRecord;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char where[160];
    DescribeAddress((uintptr_t)er->ExceptionAddress, where, sizeof(where));
    fprintf(f, "\r\n==== %s %s - %04d-%02d-%02d %02d:%02d:%02d - %s ====\r\n", COOP_NAME, COOP_VERSION_STR, st.wYear,
            st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, firstChance ? "first-chance" : "UNHANDLED");
    fprintf(f, "Exception %08X at %s\r\n", (unsigned)er->ExceptionCode, where);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(f, "  %s address %08X\r\n", er->ExceptionInformation[0] ? "write to" : "read from",
                (unsigned)er->ExceptionInformation[1]);
    fprintf(f, "EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X\r\nEBP=%08X ESP=%08X EIP=%08X EFL=%08X\r\n",
            (unsigned)ctx->Eax, (unsigned)ctx->Ebx, (unsigned)ctx->Ecx, (unsigned)ctx->Edx, (unsigned)ctx->Esi,
            (unsigned)ctx->Edi, (unsigned)ctx->Ebp, (unsigned)ctx->Esp, (unsigned)ctx->Eip, (unsigned)ctx->EFlags);
    fprintf(f, "Probable call stack (return addresses on the stack):\r\n");
    // the handler runs on the faulting thread: its stack ends at NT_TIB::StackBase
    uintptr_t stackBase = (uintptr_t)((NT_TIB*)NtCurrentTeb())->StackBase;
    uintptr_t to = std::min<uintptr_t>(stackBase, (uintptr_t)ctx->Esp + 4096);
    WriteStack(f, (const uint8_t*)ctx->Esp, to - ctx->Esp);
    fclose(f);
    LOGE("Exception %08X at %s (%s) - see crash.log", (unsigned)er->ExceptionCode, where,
         firstChance ? "first-chance" : "unhandled");
}

// The game left its main loop (RsGlobal.quit): what breaks now breaks in the teardown, after CGame::FinalShutdown.
// Plugins that keep the players alive past it (MixSets' SkipShutdown, a GPS mod's ~CPlayerPed) crashed there on the
// freed texture pool, ten "crashes" in one player's report that were the game closing.
bool Quitting() { return *(volatile uint8_t*)0xC17050 != 0; }

LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (Quitting()) return EXCEPTION_CONTINUE_SEARCH;
    // the game's and ours only: drivers and plugins take and handle their own (IsBadReadPtr), the reports ran out on them
    uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    bool ours = (at >= g_exeBase && at < g_exeEnd) || (at >= g_selfBase && at < g_selfEnd);
    if (ours && (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                 code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_PRIV_INSTRUCTION || code == STATUS_HEAP_CORRUPTION)) {
        WriteReport(ep, true);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
    // a plugin's filter put over ours may call ours as its previous one (ReclaimCrashFilter): no loop
    if (InterlockedExchange(&g_inFilter, 1)) return EXCEPTION_CONTINUE_SEARCH;
    if (Quitting()) {
        LOGW("The game crashed while closing (exception %08X): it was closing anyway", (unsigned)ep->ExceptionRecord->ExceptionCode);
        TerminateProcess(GetCurrentProcess(), 0);
    }
    WriteReport(ep, false);
    LONG r = g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
    // nobody took it: Windows Error Reporting comes next, and its window may never show for a fullscreen game. The game
    // was gone while its process stayed, with the server it started still on the port (the next game connected to that
    // old server: "wrong password"). The report is written: the process ends here.
    if (r == EXCEPTION_CONTINUE_SEARCH) TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord->ExceptionCode);
    return r;
}

// CStreaming's two read channels (ms_channel 0x8E4A60, 0x98 bytes each; ms_channelError 0x8E4B90) and the first
// model each one reads (CStreamingInfo 0x14 bytes from 0x8E4CC0: img +7, sector +8, size +0xC). A read that keeps
// failing holds the game in CStreaming::RetryLoadFile for good: these name the file it waits for.
void WriteStreaming(FILE* f) {
    fprintf(f, "Streaming: channel error %d\r\n", *(volatile int32_t*)0x8E4B90);
    for (int c = 0; c < 2; ++c) {
        auto* ch = (volatile const uint8_t*)(0x8E4A60 + c * 0x98);
        int model = *(volatile const int32_t*)ch;
        fprintf(f, "  channel %d: state %d, sector %08X, %d sectors, tries %d, status %d, model %d", c,
                *(volatile const int32_t*)(ch + 0x80), (unsigned)*(volatile const int32_t*)(ch + 0x88),
                *(volatile const int32_t*)(ch + 0x8C), *(volatile const int32_t*)(ch + 0x90), *(volatile const int32_t*)(ch + 0x94),
                model);
        if (model >= 0 && model < 26316) {
            auto* info = (volatile const uint8_t*)(0x8E4CC0 + model * 0x14);
            fprintf(f, " (img %u, sector %d, size %d, state %u)", info[7], *(volatile const int32_t*)(info + 8),
                    *(volatile const int32_t*)(info + 0xC), info[0x10]);
        }
        fprintf(f, "\r\n");
    }
}

void DumpThread(DWORD tid, const char* why) {
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th) return;
    // Only the registers and a copy of the stack while it stands: it may hold the heap's, the CRT's or the loader's lock
    // (a long load is where this trips), and the file and module calls below waited for it forever.
    static uint8_t stack[8192];
    size_t size = 0;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    SuspendThread(th);
    bool got = GetThreadContext(th, &ctx) != 0;
    MEMORY_BASIC_INFORMATION mbi{};
    if (got && VirtualQuery((void*)ctx.Esp, &mbi, sizeof(mbi))) {
        size = std::min<uintptr_t>((uintptr_t)mbi.BaseAddress + mbi.RegionSize - ctx.Esp, sizeof(stack));
        memcpy(stack, (void*)ctx.Esp, size);
    }
    ResumeThread(th);
    CloseHandle(th);
    if (!got) return;
    char where[160];
    DescribeAddress(ctx.Eip, where, sizeof(where));
    if (FILE* f = _wfopen(g_crashPath.c_str(), L"ab")) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "\r\n==== %s - %02d:%02d:%02d - main thread %s ====\r\nEIP %s ESP=%08X EBP=%08X\r\n", COOP_NAME,
                st.wHour, st.wMinute, st.wSecond, why, where, (unsigned)ctx.Esp, (unsigned)ctx.Ebp);
        WriteStack(f, stack, size);
        WriteStreaming(f);
        fclose(f);
    }
    LOGE("Main thread %s at %s - stack written to crash.log", why, where);
}

DWORD WINAPI HangThread(void*) {
    LONG last = g_heartbeat;
    unsigned still = 0;
    bool reported = false;
    for (;;) {
        Sleep(1000);
        LONG now = g_heartbeat;
        if (now != last) {
            last = now;
            still = 0;
            reported = false;
            continue;
        }
        if (++still >= g_hangSeconds && !reported) {
            reported = true;
            // a minimized game waits in WinMain's loop and draws no frames: no hang, though it looked like one (#97)
            uintptr_t ps = *(const uintptr_t*)0xC17054;  // RsGlobal.ps, its window first
            HWND window = ps ? *(const HWND*)ps : nullptr;
            DumpThread(g_mainThreadId, window && IsIconic(window) ? "not responding (its window minimized)" : "not responding");
        }
    }
}
}  // namespace

void Heartbeat() { InterlockedIncrement(&g_heartbeat); }

void StartHangDetector(unsigned seconds) {
    g_hangSeconds = seconds;
    g_mainThreadId = GetCurrentThreadId();
    CreateThread(nullptr, 0, HangThread, nullptr, 0, nullptr);
}

void InstallCrashHandler(const std::wstring& coopDir) {
    g_crashPath = coopDir + L"crash.log";
    ModuleRange(GetModuleHandleW(nullptr), g_exeBase, g_exeEnd);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&InstallCrashHandler, &self);
    ModuleRange(self, g_selfBase, g_selfEnd);
    AddVectoredExceptionHandler(0, VectoredHandler);
    g_prevFilter = SetUnhandledExceptionFilter(UnhandledFilter);
}

void ReclaimCrashFilter() {
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(UnhandledFilter);
    if (prev == UnhandledFilter) return;
    g_prevFilter = prev;  // theirs runs after ours
    LOGI("A plugin loaded later had taken the crash filter: coop's runs first again");
}

}  // namespace coop
