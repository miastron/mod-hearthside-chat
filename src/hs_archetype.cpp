#include "hs_archetype.h"

#include <array>
#include <cstdint>

namespace
{
    // SplitMix64's finalizer — same mixer hs_style.cpp uses for the same
    // reason: AzerothCore GUIDs come from a small sequential counter, so
    // std::hash<uint64_t> alone (identity on libstdc++) would draw
    // neighbouring GUIDs into neighbouring archetypes instead of scattering
    // them across the weighted pool.
    uint64_t MixBits64(uint64_t x)
    {
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBULL;
        x ^= x >> 31;
        return x;
    }

    // Independent salt from hs_style.cpp's care-jitter mix so archetype
    // assignment doesn't correlate with which direction a bot's jitter
    // happens to land.
    constexpr uint64_t kArchetypeSalt = 0xC2B2AE3D27D4EB4FULL;

    // Enum order is the array-index contract Hs_ArchetypeInfoFor relies on --
    // hs_archetype_store.cpp matches hside_archetype rows to this list by
    // enum_name rather than trusting row order, so this is the single source
    // of truth for "which index is which archetype."
    constexpr std::array<const char*, kHsArchetypeCount> kEnumNames = {{
        "RAIDER_SERIOUS", "RAIDER_CASUAL", "PVP_SERIOUS", "PVP_CASUAL", "TRADER",
        "LOOTGOBLIN", "CASUAL", "GRUMPY_VETERAN", "LONE_WOLF", "MENTOR",
        "YOUNG_APPRENTICE", "SOCIALITE", "DISTRACTED", "TROLL_MILD", "TROLL_AGGRESSIVE",
    }};

    // Populated by Hs_SetArchetypeTable, normally called once at startup by
    // hs_archetype_store.cpp's Hs_LoadArchetypesFromDb(). Defaults to a
    // single safe entry (CASUAL at full weight, no eligibility bound) so a
    // lookup before that call — or a missing/misconfigured hside_archetype
    // table — degrades to "every bot is CASUAL" rather than reading
    // uninitialized data. hs_archetype_store.cpp logs an error on that path;
    // this file has no logging dependency of its own by design.
    std::array<HsArchetypeInfo, kHsArchetypeCount> g_Archetypes = {{
        { kEnumNames[0],  "", 0.5f, 0.5f, 30, 0, false, 0.0f, 0, 255, 0 },
        { kEnumNames[1],  "", 0.5f, 0.5f, 30, 0, false, 0.0f, 0, 255, 0 },
        { kEnumNames[2],  "", 0.5f, 0.5f, 30, 0, false, 0.0f, 0, 255, 0 },
        { kEnumNames[3],  "", 0.5f, 0.5f, 30, 0, false, 0.0f, 0, 255, 0 },
        { kEnumNames[4],  "", 0.5f, 0.5f, 30, 0, false, 0.0f, 0, 255, 0 },
        { kEnumNames[5],  "", 0.5f, 0.5f, 30, 0, false, 0.0f, 0, 255, 0 },
        { kEnumNames[6],  "whatever is in front of them", 0.45f, 0.55f, 30, 100, false, 0.0f, 0, 255, 0 }, // CASUAL -- the one real fallback row, weight 100 so it's always drawn until the DB table loads
        { kEnumNames[7],  "", 0.5f, 0.5f, 30, 0, false, 0.0f, 0, 255, 0 },
        { kEnumNames[8],  "", 0.5f, 0.5f, 30, 0, false, 0.0f, 0, 255, 0 },
        { kEnumNames[9],  "", 0.5f, 0.5f, 30, 0, false, 0.0f, 0, 255, 0 },
        { kEnumNames[10], "", 0.5f, 0.5f, 30, 0, false, 0.0f, 0, 255, 0 },
        { kEnumNames[11], "", 0.5f, 0.5f, 30, 0, false, 0.0f, 0, 255, 0 },
        { kEnumNames[12], "", 0.5f, 0.5f, 30, 0, false, 0.0f, 0, 255, 0 },
        { kEnumNames[13], "", 0.5f, 0.5f, 25, 0, false, 0.0f, 0, 255, 1 },
        { kEnumNames[14], "", 0.5f, 0.5f, 30, 0, false, 0.0f, 0, 255, 2 },
    }};

    bool IsEligibleForLevel(const HsArchetypeInfo& info, uint8_t level)
    {
        return level >= info.minLevel && level <= info.maxLevel;
    }
}

void Hs_SetArchetypeTable(const std::array<HsArchetypeInfo, kHsArchetypeCount>& table)
{
    g_Archetypes = table;
}

const HsArchetypeInfo& Hs_ArchetypeInfoFor(HsArchetype a)
{
    return g_Archetypes[static_cast<size_t>(a)];
}

HsArchetype Hs_ArchetypeForBot(uint64_t botGuid, uint8_t level)
{
    uint64_t h = MixBits64(botGuid ^ kArchetypeSalt);

    uint32_t eligibleTotal = 0;
    for (auto const& info : g_Archetypes)
        if (IsEligibleForLevel(info, level))
            eligibleTotal += info.spawnWeight;

    // The safety-default table above (single CASUAL at weight 100, no level
    // bound) guarantees eligibleTotal is nonzero even before a real table
    // loads; the seeded hside_archetype table keeps at least six
    // no-level-requirement entries, so this stays provably nonzero for
    // every level 1-80 once loaded too. This guard is defensive, not
    // reachable in practice.
    if (eligibleTotal == 0)
        return HsArchetype::Casual;

    uint32_t roll = static_cast<uint32_t>(h % static_cast<uint64_t>(eligibleTotal));

    uint32_t cumulative = 0;
    for (size_t i = 0; i < g_Archetypes.size(); ++i)
    {
        if (!IsEligibleForLevel(g_Archetypes[i], level))
            continue;
        cumulative += g_Archetypes[i].spawnWeight;
        if (roll < cumulative)
            return static_cast<HsArchetype>(i);
    }
    return HsArchetype::Casual; // unreachable if the loop above is correct
}

std::string Hs_ArchetypePromptLine(HsArchetype a)
{
    const HsArchetypeInfo& info = Hs_ArchetypeInfoFor(a);
    std::string line = std::string("You mostly talk about: ") + info.talksAbout + ".";

    // New 2026-08-21: TROLL_MILD/TROLL_AGGRESSIVE's profanity axis. Scoped
    // to the game deliberately -- gear, rotations, loot, other players'
    // choices -- never the real person on the other end. That boundary is
    // part of the generated prompt text itself so it travels with every
    // request, not left to the model's judgement or to a post-hoc filter.
    if (info.profanityLevel == 1)
        line += " You swear lightly and casually when annoyed (damn, hell, crap-tier) -- about the game, never at the person you're talking to.";
    else if (info.profanityLevel == 2)
        line += " You swear openly and vulgarly when annoyed or mocking someone's gameplay -- about the game, gear, or choices, never about the real person you're talking to.";

    return line;
}
