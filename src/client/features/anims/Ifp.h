#pragma once
// A player's own animation file (coop\anims\*.ifp) read through before the game takes it: CAnimManager::LoadAnimFile trusts
// every count and size in it, and a damaged or foreign file would be written past its buffers. Only the game's own
// formats of San Andreas, ANP3 and ANP2 (the same without the frame sizes). Header-only for its unit test.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::anims {

struct IfpInfo {
    std::string block;               // the block name written in the file
    std::vector<std::string> anims;  // its animations, in order
};

// true: every animation of the file reads within its bytes, with sane counts and key times in order (a key time going
// back loops the game's CAnimBlendNode::NextKeyFrame for ever)
inline bool CheckIfp(const uint8_t* b, size_t n, IfpInfo& out) {
    out = {};
    if (n < 36 || (memcmp(b, "ANP3", 4) && memcmp(b, "ANP2", 4))) return false;
    bool anp3 = !memcmp(b, "ANP3", 4);
    auto u32 = [&](size_t at) {
        uint32_t v;
        memcpy(&v, b + at, 4);
        return v;
    };
    out.block.assign((const char*)b + 8, strnlen((const char*)b + 8, 24));
    uint32_t count = u32(32);
    if (count == 0 || count > 1024) return false;
    size_t pos = 36;
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 28 + (anp3 ? 8 : 0) > n) return false;
        out.anims.emplace_back((const char*)b + pos, strnlen((const char*)b + pos, 24));
        uint32_t seqs = u32(pos + 24), size = anp3 ? u32(pos + 28) : 0, flags = anp3 ? u32(pos + 32) : 0;
        pos += 28 + (anp3 ? 8 : 0);
        if (out.anims.back().empty() || seqs == 0 || seqs > 128) return false;
        uint64_t used = 0;
        for (uint32_t s = 0; s < seqs; ++s) {
            if (pos + 36 > n) return false;
            uint32_t type = u32(pos + 24), frames = u32(pos + 28);
            pos += 36;
            // KeyFrame, KeyFrameTrans and their compressed forms; the compressed ones only in a compressed animation
            static const uint32_t kSize[] = {0, 20, 32, 10, 16};
            if (type < 1 || type > 4 || frames == 0 || frames > 100000 || (anp3 && ((flags & 1) != 0) != (type >= 3)))
                return false;
            uint64_t bytes = (uint64_t)kSize[type] * frames;
            if (pos + bytes > n) return false;
            float last = 0.0f;
            for (uint32_t k = 0; k < frames; ++k) {
                const uint8_t* key = b + pos + (size_t)k * kSize[type];
                float t;
                if (type >= 3) {
                    int16_t c;
                    memcpy(&c, key + 8, 2);
                    t = c / 60.0f;
                } else {
                    memcpy(&t, key + (type == 1 ? 16 : 28), 4);
                }
                if (!std::isfinite(t) || t < last || t > 3600.0f) return false;
                last = t;
            }
            pos += (size_t)bytes;
            used += bytes;
        }
        if (anp3 && used > size) return false;  // the loader reads an animation's frames into one block of `size` bytes
    }
    return true;
}

}  // namespace coop::anims
