#ifndef MOD_HS_HASH_H
#define MOD_HS_HASH_H

#include <cstdint>
#include <functional>
#include <string>

// The module's deterministic-choice hashing, in one place.
//
// Five files carried a byte-identical copy of the mixer below
// (hs_archetype.cpp, hs_channel.cpp, hs_grounded.cpp, hs_reflex.cpp,
// hs_style.cpp) and three carried the same message seed, each one's comments
// naming the others -- known duplication that nothing would catch a
// divergence in (review item 17). Header-only and free of AzerothCore
// includes, so the Tests/ harnesses for all five still build standalone.
namespace HsHash
{
    // SplitMix64's finalizer. AzerothCore GUIDs come off a small sequential
    // counter and std::hash<uint64_t> is the identity function on libstdc++,
    // so hashing a GUID directly draws neighbouring GUIDs into neighbouring
    // buckets -- adjacent archetypes, correlated style jitter. This gives
    // every input a full-avalanche 64-bit spread regardless of what the
    // platform's std::hash does.
    //
    // Callers salt their own input (kArchetypeSalt, kChannelSalt, the reflex
    // families' two salts) so that independent decisions about the same bot
    // do not correlate with each other.
    inline uint64_t Hs_MixBits64(uint64_t x)
    {
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBULL;
        x ^= x >> 31;
        return x;
    }

    // hash(botGuid, message text): a seed that is stable for a given (bot,
    // message) pair and uncorrelated across either. Not cryptographic;
    // reproducibility is all it needs to be.
    inline uint64_t Hs_SeedForMessage(uint64_t botGuid, const std::string& text)
    {
        uint64_t h = std::hash<std::string>{}(text);
        h ^= Hs_MixBits64(botGuid) + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        return h;
    }
}

#endif // MOD_HS_HASH_H
