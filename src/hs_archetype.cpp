#include "hs_archetype.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>

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
        "LOOTGOBLIN", "CASUAL", "GRUMPY_VETERAN", "MENTOR",
        "YOUNG_APPRENTICE", "SOCIALITE", "TROLL_MILD", "TROLL_AGGRESSIVE",
    }};

    // Populated by Hs_SetArchetypeTable, normally called once at startup by
    // hs_archetype_store.cpp's Hs_LoadArchetypesFromDb(). Defaults to a
    // single safe entry (CASUAL at full weight, no eligibility bound) so a
    // lookup before that call -- or a missing/misconfigured hside_archetype
    // table -- degrades to "every bot is CASUAL" rather than reading
    // uninitialized data. hs_archetype_store.cpp logs an error on that path;
    // this file has no logging dependency of its own by design.
    std::array<HsArchetypeInfo, kHsArchetypeCount> g_Archetypes = {{
        { kEnumNames[0],  "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 255, 0, 800, 45 },
        { kEnumNames[1],  "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 255, 0, 800, 45 },
        { kEnumNames[2],  "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 255, 0, 800, 45 },
        { kEnumNames[3],  "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 255, 0, 800, 45 },
        { kEnumNames[4],  "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 255, 0, 800, 45 },
        { kEnumNames[5],  "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 255, 0, 800, 45 },
        { kEnumNames[6],  "whatever is in front of them", 0.45f, 0.0f, 30, 100, false, 0.0f, 0, 255, 0, 800, 45 }, // CASUAL -- the one real fallback row, weight 100 so it's always drawn until the DB table loads
        { kEnumNames[7],  "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 255, 0, 800, 45 },
        { kEnumNames[8],  "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 255, 0, 800, 45 },
        { kEnumNames[9],  "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 255, 0, 800, 45 },
        { kEnumNames[10], "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 255, 0, 800, 45 },
        { kEnumNames[11], "", 0.5f, 0.0f, 25, 0, false, 0.0f, 0, 255, 1, 800, 45 },
        { kEnumNames[12], "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 255, 2, 800, 45 },
    }};

    bool IsEligibleForLevel(const HsArchetypeInfo& info, uint8_t level)
    {
        return level >= info.minLevel && level <= info.maxLevel;
    }

    // GM-set pins (hs_command.cpp's `.hearthside archetype`). Own mutex,
    // separate from g_ArchetypeTableMutex, since this map is written on
    // demand from the world thread and read from both the world and worker
    // threads.
    std::mutex                                  g_ArchetypeOverrideMutex;
    std::unordered_map<uint64_t, HsArchetype>   g_ArchetypeOverrides;

    // Guards every access to g_Archetypes. `.reload config` replaces the
    // whole table from the world thread (hs_main.cpp's
    // HsArchetypeLifecycleWorldScript) while the queue-worker and generator
    // threads are reading it -- and each HsArchetypeInfo owns a
    // std::string talksAbout, so an unguarded replace frees a buffer a
    // reader may be mid-way through. Hs_ArchetypeInfoFor returns by value
    // for the same reason: its callers hold the result across a
    // multi-second Hs_CallLLM, long after any lock could still be held.
    std::mutex                                  g_ArchetypeTableMutex;
}

void Hs_SetArchetypeTable(const std::array<HsArchetypeInfo, kHsArchetypeCount>& table)
{
    std::lock_guard<std::mutex> lock(g_ArchetypeTableMutex);
    g_Archetypes = table;
}

HsArchetypeInfo Hs_ArchetypeInfoFor(HsArchetype a)
{
    std::lock_guard<std::mutex> lock(g_ArchetypeTableMutex);
    return g_Archetypes[static_cast<size_t>(a)];
}

bool Hs_ArchetypeForName(const std::string& enumName, HsArchetype& out)
{
    std::lock_guard<std::mutex> lock(g_ArchetypeTableMutex);
    for (size_t i = 0; i < g_Archetypes.size(); ++i)
    {
        if (enumName == g_Archetypes[i].enumName)
        {
            out = static_cast<HsArchetype>(i);
            return true;
        }
    }
    return false;
}

void Hs_SetArchetypeOverride(uint64_t botGuid, HsArchetype archetype)
{
    std::lock_guard<std::mutex> lock(g_ArchetypeOverrideMutex);
    g_ArchetypeOverrides[botGuid] = archetype;
}

void Hs_ClearArchetypeOverride(uint64_t botGuid)
{
    std::lock_guard<std::mutex> lock(g_ArchetypeOverrideMutex);
    g_ArchetypeOverrides.erase(botGuid);
}

HsArchetype Hs_ArchetypeForBot(uint64_t botGuid, uint8_t level)
{
    {
        std::lock_guard<std::mutex> lock(g_ArchetypeOverrideMutex);
        auto it = g_ArchetypeOverrides.find(botGuid);
        if (it != g_ArchetypeOverrides.end())
            return it->second;
    }

    uint64_t h = MixBits64(botGuid ^ kArchetypeSalt);

    std::lock_guard<std::mutex> lock(g_ArchetypeTableMutex);

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
    HsArchetypeInfo const info = Hs_ArchetypeInfoFor(a);
    std::string line = std::string("You mostly talk about: ") + info.talksAbout + ".";

    // TROLL_MILD/TROLL_AGGRESSIVE's profanity axis. No longer scoped away
    // from the person being talked to (dropped 2026-08-24 -- the archetype
    // voices already keep this in range on their own, and the arbiter's
    // rate limits/cooldowns make a sustained pile-on structurally
    // impossible, so the old boundary text was buying nothing). See
    // Claude/PLAN-TUNING.md §3's profanity bullet for the full reasoning.
    if (info.profanityLevel == 1)
        line += " You swear lightly and casually when annoyed (damn, hell, crap-tier).";
    else if (info.profanityLevel == 2)
        line += " You swear openly and vulgarly when annoyed or mocking someone's gameplay.";

    return line;
}
