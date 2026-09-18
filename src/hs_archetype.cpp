#include "hs_archetype.h"
#include "hs_hash.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace
{
    // Independent salt from hs_style.cpp's care-jitter mix so archetype
    // assignment doesn't correlate with which direction a bot's jitter
    // happens to land.
    constexpr uint64_t kArchetypeSalt = 0xC2B2AE3D27D4EB4FULL;

    // Enum order is the array-index contract Hs_ArchetypeInfoFor relies on:
    // hs_archetype_store.cpp matches hside_archetype rows to this list by
    // enum_name rather than trusting row order, so this is the single source
    // of truth for "which index is which archetype."
    constexpr std::array<const char*, kHsArchetypeCount> kEnumNames = {{
        "RAIDER_SERIOUS", "RAIDER_CASUAL", "PVP_SERIOUS", "PVP_CASUAL", "TRADER",
        "CASUAL", "GRUMPY_VETERAN", "MENTOR",
        "YOUNG_APPRENTICE", "SOCIALITE", "TROLL_MILD", "TROLL_AGGRESSIVE",
    }};

    // Populated by Hs_SetArchetypeTable, normally called once at startup by
    // hs_archetype_store.cpp's Hs_LoadArchetypesFromDb(). Defaults to a
    // single safe entry (CASUAL at full weight) so a
    // lookup before that call, or a missing/misconfigured hside_archetype
    // table, degrades to "every bot is CASUAL" rather than reading
    // uninitialized data. hs_archetype_store.cpp logs an error on that path;
    // this file has no logging dependency of its own by design.
    std::array<HsArchetypeInfo, kHsArchetypeCount> g_Archetypes = {{
        { kEnumNames[0],  "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 800, 45 },
        { kEnumNames[1],  "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 800, 45 },
        { kEnumNames[2],  "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 800, 45 },
        { kEnumNames[3],  "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 800, 45 },
        { kEnumNames[4],  "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 800, 45 },
        { kEnumNames[5],  "whatever is in front of them", 0.45f, 0.0f, 30, 100, false, 0.0f, 0, 800, 45 }, // CASUAL: the one real fallback row, weight 100 so it's always drawn until the DB table loads
        { kEnumNames[6],  "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 800, 45 },
        { kEnumNames[7],  "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 800, 45 },
        { kEnumNames[8],  "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 800, 45 },
        { kEnumNames[9],  "", 0.5f, 0.0f, 30, 0, false, 0.0f, 0, 800, 45 },
        { kEnumNames[10], "", 0.5f, 0.0f, 25, 0, false, 0.0f, 1, 800, 45 },
        { kEnumNames[11], "", 0.5f, 0.0f, 30, 0, false, 0.0f, 2, 800, 45 },
    }};

    // GM-set pins (hs_command.cpp's `.hearthside archetype`). Own mutex,
    // separate from g_ArchetypeTableMutex, since this map is written on
    // demand from the world thread and read from both the world and worker
    // threads.
    std::mutex                                  g_ArchetypeOverrideMutex;
    std::unordered_map<uint64_t, HsArchetype>   g_ArchetypeOverrides;

    // Guards every access to g_Archetypes. `.reload config` replaces the
    // whole table from the world thread (hs_main.cpp's
    // HsArchetypeLifecycleWorldScript) while the queue-worker and generator
    // threads are reading it, and each HsArchetypeInfo owns a
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

HsArchetype Hs_ArchetypeForBot(uint64_t botGuid)
{
    {
        std::lock_guard<std::mutex> lock(g_ArchetypeOverrideMutex);
        auto it = g_ArchetypeOverrides.find(botGuid);
        if (it != g_ArchetypeOverrides.end())
            return it->second;
    }

    uint64_t h = HsHash::Hs_MixBits64(botGuid ^ kArchetypeSalt);

    std::lock_guard<std::mutex> lock(g_ArchetypeTableMutex);

    // Review C1: one draw over the whole table, no level-eligible subset.
    // Because neither the modulus nor the cumulative ordering depends on
    // anything but the GUID any more, a bot's archetype is fixed for the
    // life of the character -- which is the property the identity system
    // was already assuming when it generated a card_voice from
    // archetypeInfo.talksAbout once and never regenerated it.
    uint32_t total = 0;
    for (auto const& info : g_Archetypes)
        total += info.spawnWeight;

    // The safety-default table above (single CASUAL at weight 100)
    // guarantees this is nonzero even before a real table loads, and the
    // seeded hside_archetype weights sum to exactly 100. Defensive, not
    // reachable in practice.
    if (total == 0)
        return HsArchetype::Casual;

    uint32_t roll = static_cast<uint32_t>(h % static_cast<uint64_t>(total));

    uint32_t cumulative = 0;
    for (size_t i = 0; i < g_Archetypes.size(); ++i)
    {
        cumulative += g_Archetypes[i].spawnWeight;
        if (roll < cumulative)
            return static_cast<HsArchetype>(i);
    }
    return HsArchetype::Casual; // unreachable if the loop above is correct
}

// The bare training tag, not a description of the archetype -- 2026-09-14.
//
// This module targets a model fine-tuned on Claude/finetune/*.jsonl, and the
// runtime prompt's job is to be the shape that model was trained on, not to
// explain anything to a general-purpose model. All 1949 archetype-tagged
// training rows carry exactly "Archetype: <ENUM_NAME>" and nothing else, so
// that is what ships.
//
// Until now this emitted the v1-era prose ("You mostly talk about: ...")
// plus a profanity clause -- a form the tune has never once seen, which meant
// the archetype fine-tune was not reaching players at all. Measured against
// the live endpoint with Tests/opener_diversity.py's `live` and `trained`
// frames, scored by Tests/score_voice_transfer.py (N=24/archetype, 576 calls):
//
//   prose (what this used to emit)  rank correlation of reply length
//                                   against _FORMAT.md's R5 bands: -0.32,
//                                   i.e. none. 1.9 words of spread across the
//                                   whole roster -- one house voice. MENTOR
//                                   sat at 0% inside its own 9-15 band.
//   bare tag (what it emits now)    +0.96, 8.9 words of spread, MENTOR 58%
//                                   in band, SOCIALITE 0% -> 75%.
//
// The profanity clause is gone for the same reason: profanity_level is
// trained *through* the tag (_FORMAT.md R9 -- "the tag has to be worth
// reading"), so restating it in prose is both off-shape and redundant. The
// column stays the source of truth for the dataset; it is simply not
// something the runtime prompt says out loud any more.
std::string Hs_ArchetypePromptLine(HsArchetype a)
{
    return std::string("Archetype: ") + Hs_ArchetypeInfoFor(a).enumName;
}
