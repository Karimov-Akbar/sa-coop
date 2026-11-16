#pragma once
// Memory patching and function hooking helpers for gta_sa.exe 1.0 US.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace coop::mem {

bool Init();      // MH_Initialize
void Shutdown();  // disable & remove all hooks

// Writes bytes with VirtualProtect, flushes the instruction cache.
void Write(uintptr_t addr, const void* data, size_t size);
template <typename T>
inline void Put(uintptr_t addr, T value) {
    Write(addr, &value, sizeof(T));
}
template <typename T>
inline T Get(uintptr_t addr) {
    return *reinterpret_cast<T*>(addr);
}
void Nop(uintptr_t addr, size_t size);
// Writes "jmp target" (5 bytes) at addr.
void Jump(uintptr_t addr, const void* target);
// Writes "ret N" at addr (useful to disable a function).
void Ret(uintptr_t addr, uint16_t popBytes = 0);

// Absolute target of a "call rel32" / "jmp rel32" instruction located at addr, or 0.
uintptr_t CallTarget(uintptr_t addr);

// Redirects an existing "call rel32" at addr to newTarget. The previous call target (which may
// already be another mod's hook) is stored in *previous so the detour can chain to it.
// Returns false if there is no E8 call at the address.
bool RedirectCall(uintptr_t addr, const void* newTarget, void** previous);

// Inline hook through MinHook. *original receives the trampoline.
bool Hook(uintptr_t addr, void* detour, void** original, const char* name);

template <typename F>
inline bool HookFn(uintptr_t addr, F detour, F* original, const char* name) {
    return Hook(addr, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original), name);
}
template <typename F>
inline bool RedirectCallFn(uintptr_t addr, F detour, F* previous) {
    return RedirectCall(addr, reinterpret_cast<const void*>(detour), reinterpret_cast<void**>(previous));
}

// Replaces a vtable slot, returns the previous function.
void* SwapVTable(void** vtable, int index, void* fn);

// Checks that the expected bytes are at addr (used to detect other versions / conflicting mods).
bool Check(uintptr_t addr, const void* expected, size_t size);

// Whether addr lies in committed executable memory (VirtualQuery: slow, for rare checks or cached answers).
bool IsCode(const void* addr);

}  // namespace coop::mem
