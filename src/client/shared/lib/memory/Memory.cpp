#include "shared/lib/memory/Memory.h"

#include <MinHook.h>
#include <windows.h>

#include "common/lib/log/Log.h"

namespace coop::mem {

bool Init() {
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        LOGE("MinHook init failed: %s", MH_StatusToString(s));
        return false;
    }
    return true;
}

void Shutdown() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

void Write(uintptr_t addr, const void* data, size_t size) {
    DWORD old;
    VirtualProtect((void*)addr, size, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)addr, data, size);
    VirtualProtect((void*)addr, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, size);
}

void Nop(uintptr_t addr, size_t size) {
    DWORD old;
    VirtualProtect((void*)addr, size, PAGE_EXECUTE_READWRITE, &old);
    memset((void*)addr, 0x90, size);
    VirtualProtect((void*)addr, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, size);
}

void Jump(uintptr_t addr, const void* target) {
    uint8_t code[5];
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)target - (addr + 5));
    memcpy(code + 1, &rel, 4);
    Write(addr, code, 5);
}

void Ret(uintptr_t addr, uint16_t popBytes) {
    if (popBytes == 0) {
        Put<uint8_t>(addr, 0xC3);
    } else {
        uint8_t code[3] = {0xC2, (uint8_t)(popBytes & 0xFF), (uint8_t)(popBytes >> 8)};
        Write(addr, code, 3);
    }
}

uintptr_t CallTarget(uintptr_t addr) {
    uint8_t op = *(uint8_t*)addr;
    if (op != 0xE8 && op != 0xE9) return 0;
    return addr + 5 + *(int32_t*)(addr + 1);
}

bool RedirectCall(uintptr_t addr, const void* newTarget, void** previous) {
    if (*(uint8_t*)addr != 0xE8) {
        LOGE("RedirectCall: no call instruction at %08X (byte %02X)", (unsigned)addr, *(uint8_t*)addr);
        return false;
    }
    if (previous) *previous = (void*)CallTarget(addr);
    int32_t rel = (int32_t)((uintptr_t)newTarget - (addr + 5));
    Write(addr + 1, &rel, 4);
    return true;
}

bool Hook(uintptr_t addr, void* detour, void** original, const char* name) {
    MH_STATUS s = MH_CreateHook((void*)addr, detour, original);
    if (s != MH_OK) {
        LOGE("Hook %s @%08X: create failed: %s", name, (unsigned)addr, MH_StatusToString(s));
        return false;
    }
    s = MH_EnableHook((void*)addr);
    if (s != MH_OK) {
        LOGE("Hook %s @%08X: enable failed: %s", name, (unsigned)addr, MH_StatusToString(s));
        return false;
    }
    LOGD("Hooked %s @%08X", name, (unsigned)addr);
    return true;
}

void* SwapVTable(void** vtable, int index, void* fn) {
    void* old = vtable[index];
    Write((uintptr_t)&vtable[index], &fn, sizeof(fn));
    return old;
}

bool Check(uintptr_t addr, const void* expected, size_t size) { return memcmp((void*)addr, expected, size) == 0; }

bool IsCode(const void* addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    return VirtualQuery(addr, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
           (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

}  // namespace coop::mem
