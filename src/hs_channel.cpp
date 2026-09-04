#include "hs_channel.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace
{
    // SplitMix64's finalizer: same mixer hs_archetype.cpp/hs_style.cpp use
    // for the same reason: AzerothCore GUIDs come from a small sequential
    // counter, so std::hash<uint64_t> alone would scatter neighbouring GUIDs
    // into neighbouring positions instead of a real shuffle.
    uint64_t MixBits64(uint64_t x)
    {
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBULL;
        x ^= x >> 31;
        return x;
    }

    // Independent salt from hs_archetype.cpp's/hs_style.cpp's own mixes.
    constexpr uint64_t kChannelSalt = 0x9E3779B97F4A7C15ULL;

    constexpr std::array<const char*, kHsChannelKindCount> kChannelNames = {{
        "Trade", "General", "LookingForGroup", "GuildRecruitment",
        "LocalDefense", "WorldDefense",
    }};

    // Populated by Hs_SetChannelPolicyTable, normally called once at startup
    // (and again on `.reload config`) by hs_config.cpp. Defaults to
    // all-Off/zero so a lookup before that call degrades to "every channel
    // is silent" rather than reading uninitialized data.
    //
    // Review B6: deliberately *not* under a mutex, and the reason is the
    // rule hs_config.h states rather than "an infrequent reload". A
    // HsChannelPolicy is an enum plus two uint32_t and owns no memory, so a
    // reload racing a read costs one message a wrong tier or rate --
    // formally UB, benign on x86-64 -- and never a freed buffer. Contrast
    // hs_archetype.cpp's g_Archetypes and hs_grounded.cpp's g_Questions/
    // g_Templates, whose rows own std::strings: those are locked, because
    // there the same race is a use-after-free.
    HsChannelPolicy g_ChannelPolicies[kHsChannelKindCount] = {};

    bool IsWordChar(char c)
    {
        return std::isalnum(static_cast<unsigned char>(c)) != 0;
    }

    // True if `needle` occurs in `haystackLower` (already lowercased) at a
    // position not flanked by another word character on either side: so
    // "wts" matches "WTS:" or "wts frost badge" but not "wtsryke" or a name
    // like "Growthspurt".
    bool ContainsWord(const std::string& haystackLower, const std::string& needle)
    {
        size_t pos = 0;
        while ((pos = haystackLower.find(needle, pos)) != std::string::npos)
        {
            bool leftOk  = (pos == 0) || !IsWordChar(haystackLower[pos - 1]);
            size_t end   = pos + needle.size();
            bool rightOk = (end >= haystackLower.size()) || !IsWordChar(haystackLower[end]);
            if (leftOk && rightOk)
                return true;
            pos += 1;
        }
        return false;
    }
}

const char* Hs_ChannelKindName(HsChannelKind kind)
{
    return kChannelNames[static_cast<size_t>(kind)];
}

void Hs_SetChannelPolicyTable(const HsChannelPolicy (&table)[kHsChannelKindCount])
{
    for (size_t i = 0; i < kHsChannelKindCount; ++i)
        g_ChannelPolicies[i] = table[i];
}

HsChannelPolicy Hs_ChannelPolicyFor(HsChannelKind kind)
{
    return g_ChannelPolicies[static_cast<size_t>(kind)];
}

bool Hs_IsWtsWtb(const std::string& text)
{
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return ContainsWord(lower, "wts") || ContainsWord(lower, "wtb") || ContainsWord(lower, "wtt");
}

std::vector<HsChannelCandidate> Hs_OrderChannelCandidates(
    std::vector<HsChannelCandidate> candidates,
    uint32_t speakerZoneId,
    uint32_t maxCandidates,
    uint64_t rngSeed)
{
    // Deterministic pseudo-random shuffle first (a stable sort by mixed
    // key), so the zone-local partition below preserves a random order
    // within each group instead of whatever arbitrary order the caller
    // handed in (ObjectAccessor::GetPlayers() is an unordered_map).
    std::stable_sort(candidates.begin(), candidates.end(),
        [rngSeed](const HsChannelCandidate& a, const HsChannelCandidate& b)
        {
            return MixBits64(a.guid ^ rngSeed ^ kChannelSalt) < MixBits64(b.guid ^ rngSeed ^ kChannelSalt);
        });

    std::stable_partition(candidates.begin(), candidates.end(),
        [speakerZoneId](const HsChannelCandidate& c) { return c.zoneId == speakerZoneId; });

    if (candidates.size() > maxCandidates)
        candidates.resize(maxCandidates);

    return candidates;
}
