#include "hs_event_affinity_store.h"
#include "hs_archetype.h"
#include "hs_event_arbiter.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "QueryResult.h"

#include <cstdint>
#include <string>
#include <vector>

void Hs_LoadEventAffinityFromDb()
{
    QueryResult result = CharacterDatabase.Query("SELECT event_type, archetype, weight FROM hside_event_affinity");
    if (!result)
    {
        // Not an error: the table authors only the exceptions, so an empty
        // one means "every archetype reacts to everything equally," which is
        // a valid (if flat) configuration. Logged at info so an operator who
        // *expected* the seed to be there can still see it isn't.
        LOG_INFO("module.hearthside",
            "[HearthsideChat] hside_event_affinity is empty -- every (event, archetype) pair "
            "weighs 1.0 and no archetype is favoured for any event.");
        Hs_SetEventAffinityTable({});
        return;
    }

    std::vector<HsEventAffinityRow> rows;
    uint32_t skipped = 0;
    do
    {
        std::string eventName     = (*result)[0].Get<std::string>();
        std::string archetypeName = (*result)[1].Get<std::string>();
        float       weight        = (*result)[2].Get<float>();

        HsEventType type;
        if (!Hs_EventTypeForName(eventName, type))
        {
            LOG_ERROR("module.hearthside",
                "[HearthsideChat] hside_event_affinity has an unrecognized event_type '{}' -- row skipped.",
                eventName);
            ++skipped;
            continue;
        }

        // Validated against the archetype table as loaded, not against a
        // second name list, same reasoning hs_archetype_store.cpp gives
        // for the override loader. This runs after
        // HsArchetypeLifecycleWorldScript::OnStartup, so the table is
        // populated by the time we get here.
        HsArchetype archetype;
        if (!Hs_ArchetypeForName(archetypeName, archetype))
        {
            LOG_ERROR("module.hearthside",
                "[HearthsideChat] hside_event_affinity references archetype '{}', which is not in "
                "hside_archetype -- row skipped.",
                archetypeName);
            ++skipped;
            continue;
        }

        if (weight < 0.0f)
        {
            // A negative weight would corrupt the cumulative selection (it
            // can make a running total move backwards past the roll), so
            // clamp rather than trust it. Zero is the intended "never
            // speaks to this event" floor and is left alone.
            LOG_ERROR("module.hearthside",
                "[HearthsideChat] hside_event_affinity row ({}, {}) has a negative weight {} -- clamped to 0.",
                eventName, archetypeName, weight);
            weight = 0.0f;
        }

        rows.push_back(HsEventAffinityRow{ type, archetypeName, weight });
    } while (result->NextRow());

    Hs_SetEventAffinityTable(rows);
    LOG_INFO("module.hearthside", "[HearthsideChat] Loaded {} event-affinity row(s) ({} skipped).",
        static_cast<uint32_t>(rows.size()), skipped);
}
