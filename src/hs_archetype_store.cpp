#include "hs_archetype_store.h"
#include "hs_archetype.h"
#include "hs_config.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "QueryResult.h"

#include <array>
#include <cstdint>

namespace
{
    // Same fixed enum-name list hs_archetype.cpp uses internally to build
    // its safety-default table -- duplicated here rather than exposed from
    // that file, since hs_archetype.h/.cpp stay dependency-free on purpose
    // (pure logic, standalone-testable) and this list is only needed by the
    // one function in this file that matches DB rows to enum slots.
    constexpr std::array<const char*, kHsArchetypeCount> kEnumNames = {{
        "RAIDER_SERIOUS", "RAIDER_CASUAL", "PVP_SERIOUS", "PVP_CASUAL", "TRADER",
        "LOOTGOBLIN", "CASUAL", "GRUMPY_VETERAN", "LONE_WOLF", "MENTOR",
        "YOUNG_APPRENTICE", "SOCIALITE", "DISTRACTED", "TROLL_MILD", "TROLL_AGGRESSIVE",
    }};
}

void Hs_LoadArchetypesFromDb()
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT enum_name, talks_about, care, reply_chance, verbosity_cap, spawn_weight, "
        "has_abbrev_override, abbrev_override_chance, min_level, max_level, profanity_level "
        "FROM hside_archetype");

    std::array<HsArchetypeInfo, kHsArchetypeCount> table;
    std::array<bool, kHsArchetypeCount> found{};
    for (size_t i = 0; i < kHsArchetypeCount; ++i)
        table[i] = HsArchetypeInfo{ kEnumNames[i], "", 0.5f, 0.5f, 30, 0, false, 0.0f, 0, 255, 0 };

    if (!result)
    {
        LOG_ERROR("server.loading",
            "[HearthsideChat] hside_archetype returned no rows -- every archetype falls back to a "
            "zero-weight placeholder (bots will draw CASUAL by Hs_ArchetypeForBot's own defensive "
            "fallback). Check that the module's base SQL installed correctly.");
        Hs_SetArchetypeTable(table);
        return;
    }

    uint32_t matched = 0;
    do
    {
        std::string enumName = (*result)[0].Get<std::string>();
        size_t slot = kHsArchetypeCount;
        for (size_t i = 0; i < kHsArchetypeCount; ++i)
        {
            if (enumName == kEnumNames[i])
            {
                slot = i;
                break;
            }
        }
        if (slot == kHsArchetypeCount)
        {
            LOG_ERROR("server.loading", "[HearthsideChat] hside_archetype has an unrecognized enum_name '{}' -- ignored.", enumName);
            continue;
        }

        HsArchetypeInfo& info = table[slot];
        info.talksAbout            = (*result)[1].Get<std::string>();
        info.care                  = (*result)[2].Get<float>();
        info.replyChance           = (*result)[3].Get<float>();
        info.verbosityCap          = (*result)[4].Get<uint32_t>();
        info.spawnWeight           = (*result)[5].Get<uint32_t>();
        info.hasAbbrevOverride     = (*result)[6].Get<bool>();
        info.abbrevOverrideChance  = (*result)[7].Get<float>();
        info.minLevel              = (*result)[8].Get<uint8_t>();
        info.maxLevel              = (*result)[9].Get<uint8_t>();
        info.profanityLevel        = (*result)[10].Get<uint8_t>();
        found[slot] = true;
        ++matched;
    } while (result->NextRow());

    for (size_t i = 0; i < kHsArchetypeCount; ++i)
    {
        if (!found[i])
            LOG_ERROR("server.loading", "[HearthsideChat] hside_archetype is missing '{}' -- it will never be drawn (weight 0) until the row is added.", kEnumNames[i]);
    }

    Hs_SetArchetypeTable(table);

    if (g_HsDebugEnabled)
        LOG_INFO("server.loading", "[HearthsideChat] Loaded {} of {} archetype row(s) from hside_archetype.", matched, kHsArchetypeCount);
}
