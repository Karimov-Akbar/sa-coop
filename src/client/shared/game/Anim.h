#pragma once
// Views over the animation system: CAnimBlendAssociation (playing instance), CAnimBlendHierarchy
// (animation data, CAnimManager::ms_aAnimations) and CAnimBlock (one IFP file, ms_aAnimBlocks).
// An animation goes between games by names: its IFP block's (BlockKey) and its own (NameKey). The indices are each
// game's own: a mod's gta3.img or ped.ifp with more or other animations shifts them, and the copies played other
// animations than their owners (a gym and a clothes shop, 03.11).
#include <cctype>
#include <cstdint>
#include <unordered_map>

#include "shared/game/Game.h"

namespace game::anim {

constexpr uintptr_t kHierarchies = 0xB4EA40;  // CAnimManager::ms_aAnimations, 0x18 bytes each
constexpr uintptr_t kBlocks = 0xB5D4A0;       // CAnimManager::ms_aAnimBlocks, 0x20 bytes each
constexpr int kIfpBase = 25575;               // streaming id of the first IFP resource
constexpr uint16_t F_LOOPED = 0x2, F_AUTO_REMOVE = 0x4, F_PARTIAL = 0x10, F_SECONDARY = 0x1000, F_BLOCK_REFERENCED = 0x4000,
                   F_INDESTRUCTIBLE = 0x8000;

inline void* First(void* clump) { return clump ? Call<0x4D15E0, void*>(clump) : nullptr; }  // RpAnimBlendClumpGetFirstAssociation
inline void* Next(void* a) { return Call<0x4D6AB0, void*>(a); }                              // RpAnimBlendGetNextAssociation
inline int16_t Group(void* a) { return Field<int16_t>(a, 0x0E); }
inline void* Hierarchy(void* a) { return Field<void*>(a, 0x14); }
inline float& BlendAmount(void* a) { return Field<float>(a, 0x18); }
inline float& BlendDelta(void* a) { return Field<float>(a, 0x1C); }
inline float CurrentTime(void* a) { return Field<float>(a, 0x20); }
inline int16_t Id(void* a) { return Field<int16_t>(a, 0x2C); }
inline uint16_t& Flags(void* a) { return Field<uint16_t>(a, 0x2E); }
inline void SetCurrentTime(void* a, float t) { CallMethod<0x4CEA80>(a, t); }
// blends it out and deletes it once out (CAnimBlendAssociation::UpdateBlend removes only auto-remove ones)
inline void FadeOut(void* a) {
    BlendDelta(a) = -4.0f;
    Flags(a) |= F_AUTO_REMOVE;
}
inline float TotalTime(void* hierarchy) { return Field<float>(hierarchy, 0x10); }

inline uintptr_t Block(int i) { return kBlocks + (uintptr_t)i * 0x20; }
inline bool BlockLoaded(int i) { return Ref<uint8_t>(Block(i) + 0x10) != 0; }

// hierarchy -> (block, index); false for animations outside the IFP blocks. A cutscene's animations are in a
// temporary last block (CCutsceneMgr loads them with LoadAnimFile): the same index means another one elsewhere.
inline bool Locate(void* h, int& block, int& idx) {
    if (!h) return false;
    block = Field<int32_t>(h, 0x0C);
    if (block < 0 || block >= Ref<int32_t>(0xB4EA30)) return false;  // ms_numAnimBlocks
    intptr_t off = (intptr_t)h - (intptr_t)(kHierarchies + (uintptr_t)Ref<int32_t>(Block(block) + 0x14) * 0x18);
    idx = (int)(off / 0x18);
    return off >= 0 && off % 0x18 == 0 && idx < Ref<int32_t>(Block(block) + 0x18);
}

// CAnimBlendHierarchy::m_hashKey: the key of the animation's name (CKeyGen::GetUppercaseKey), the same in every game
inline uint32_t NameKey(void* h) { return Field<uint32_t>(h, 0x00); }

// the block's name as a key (any case, as CAnimManager::GetAnimationBlockIndex compares them), never 0
inline uint32_t BlockKey(int block) {
    const char* s = (const char*)Block(block);
    uint32_t k = 2166136261u;
    for (int i = 0; i < 16 && s[i]; ++i) k = (k ^ (uint8_t)tolower((uint8_t)s[i])) * 16777619u;
    return k ? k : 1;
}

// the block of that name here, -1: none (0, or an IFP this game has not)
inline int BlockByKey(uint32_t key) {
    static std::unordered_map<uint32_t, int> blocks;
    static int counted = -1;
    int n = Ref<int32_t>(0xB4EA30);  // ms_numAnimBlocks: our own files' blocks come after the game's
    if (n != counted) {
        blocks.clear();
        for (int b = 0; b < n; ++b) blocks.emplace(BlockKey(b), b);
        counted = n;
    }
    auto it = blocks.find(key);
    return key && it != blocks.end() ? it->second : -1;
}

// (block, name key) -> hierarchy. The IFP is requested (kept in memory) when it is not loaded yet: nullptr then, as for
// a name this game's file does not have.
inline void* Find(int block, uint32_t key) {
    if (block < 0 || block >= Ref<int32_t>(0xB4EA30)) return nullptr;
    if (!BlockLoaded(block)) {
        RequestModel(kIfpBase + block, 2);
        return nullptr;
    }
    uintptr_t first = kHierarchies + (uintptr_t)Ref<int32_t>(Block(block) + 0x14) * 0x18;
    for (int i = 0, n = Ref<int32_t>(Block(block) + 0x18); i < n; ++i)
        if (Ref<uint32_t>(first + (uintptr_t)i * 0x18) == key) return (void*)(first + (uintptr_t)i * 0x18);
    return nullptr;
}

// CAnimManager::BlendAnimation(clump, hierarchy, flags, delta). The association references the IFP
// itself, so a copied BLOCK_REFERENCED flag would release that reference twice; an indestructible
// copy would never be freed after blending out.
inline void* Blend(void* clump, void* h, uint16_t flags, float delta) {
    return Call<0x4D4410, void*>(clump, h, (int)(flags & ~(F_BLOCK_REFERENCED | F_INDESTRUCTIBLE)), delta);
}

// The animation a ped is seen playing: a script's (TASK_PLAY_ANIM blends it by name, with no group, as a partial one laid
// over the rest) before the strongest full-body one. A face's, an arm's and a flinch are partial too, each of a group or
// flagged so. With full-body ones only, a copy stood upright at the pool table while its person aimed and struck: POOL's
// are all partial (28.10).
inline void* Main(void* clump) {
    void *named = nullptr, *body = nullptr;
    for (void* a = First(clump); a; a = Next(a)) {
        int block, idx;
        if (BlendDelta(a) < 0.0f || !Locate(Hierarchy(a), block, idx)) continue;
        bool partial = (Flags(a) & F_PARTIAL) != 0;
        if (partial && (Group(a) >= 0 || (Flags(a) & (F_SECONDARY | F_INDESTRUCTIBLE)))) continue;
        void*& best = partial ? named : body;
        if (!best || BlendAmount(a) > BlendAmount(best)) best = a;
    }
    return named ? named : body;
}

}  // namespace game::anim
