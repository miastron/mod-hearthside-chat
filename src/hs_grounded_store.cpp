#include "hs_grounded_store.h"
#include "hs_grounded.h"
#include "hs_config.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "QueryResult.h"

#include <string>
#include <vector>

namespace
{
    // hs_grounded.h/.cpp stay dependency-free on purpose (pure logic,
    // standalone-testable) so this name<->enum table is duplicated here
    // rather than exposed from there, same precedent as
    // hs_archetype_store.cpp's kEnumNames.
    struct KindName
    {
        const char*    name;
        HsGroundedKind kind;
    };

    constexpr KindName kKindNames[] = {
        { "MOUNT",          HsGroundedKind::Mount },
        { "LEVEL",          HsGroundedKind::Level },
        { "GOLD",           HsGroundedKind::Gold },
        { "ZONE",           HsGroundedKind::Zone },
        { "GUILD",          HsGroundedKind::Guild },
        { "PROFESSION",     HsGroundedKind::Profession },
        { "GEAR",           HsGroundedKind::Gear },
        { "CURRENT_GOAL",   HsGroundedKind::CurrentGoal },
        { "PLAYED_SINCE",   HsGroundedKind::PlayedSince },
        { "ALT",            HsGroundedKind::Alt },
        { "RECALL_MET",     HsGroundedKind::RecallMet },
        { "RECALL_DUNGEON", HsGroundedKind::RecallDungeon },
        { "RECALL_GROUPED", HsGroundedKind::RecallGrouped },
    };

    bool KindForName(const std::string& name, HsGroundedKind& out)
    {
        for (auto const& entry : kKindNames)
        {
            if (name == entry.name)
            {
                out = entry.kind;
                return true;
            }
        }
        return false;
    }
}

void Hs_LoadGroundedQuestionsFromDb()
{
    QueryResult result = CharacterDatabase.Query("SELECT kind, phrase FROM hside_grounded_question");
    if (!result)
    {
        LOG_ERROR("server.loading",
            "[HearthsideChat] hside_grounded_question returned no rows -- Hs_MatchGroundedQuestion "
            "will never match anything until it's populated. Check that the module's base SQL "
            "installed correctly.");
        Hs_SetGroundedQuestionTable({});
        return;
    }

    std::vector<HsGroundedQuestionRow> rows;
    uint32_t skipped = 0;
    do
    {
        std::string kindName = (*result)[0].Get<std::string>();
        std::string phrase   = (*result)[1].Get<std::string>();

        HsGroundedKind kind;
        if (!KindForName(kindName, kind))
        {
            LOG_ERROR("server.loading",
                "[HearthsideChat] hside_grounded_question has an unrecognized kind '{}' -- row skipped.",
                kindName);
            ++skipped;
            continue;
        }
        rows.push_back({ kind, phrase });
    } while (result->NextRow());

    Hs_SetGroundedQuestionTable(rows);

    if (g_HsDebugEnabled)
        LOG_INFO("server.loading",
            "[HearthsideChat] Loaded {} hside_grounded_question row(s) ({} skipped).", rows.size(), skipped);
}

void Hs_LoadGroundedTemplatesFromDb()
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT kind, has_fact, uses_fact, prefix, suffix FROM hside_grounded_template");
    if (!result)
    {
        LOG_ERROR("server.loading",
            "[HearthsideChat] hside_grounded_template returned no rows -- every matched grounded "
            "question will build an empty reply and fall through. Check that the module's base SQL "
            "installed correctly.");
        Hs_SetGroundedTemplateTable({});
        return;
    }

    std::vector<HsGroundedTemplateRow> rows;
    uint32_t skipped = 0;
    do
    {
        std::string kindName = (*result)[0].Get<std::string>();
        bool        hasFact  = (*result)[1].Get<bool>();
        bool        usesFact = (*result)[2].Get<bool>();
        std::string prefix   = (*result)[3].Get<std::string>();
        std::string suffix   = (*result)[4].Get<std::string>();

        HsGroundedKind kind;
        if (!KindForName(kindName, kind))
        {
            LOG_ERROR("server.loading",
                "[HearthsideChat] hside_grounded_template has an unrecognized kind '{}' -- row skipped.",
                kindName);
            ++skipped;
            continue;
        }
        rows.push_back({ kind, hasFact, usesFact, prefix, suffix });
    } while (result->NextRow());

    Hs_SetGroundedTemplateTable(rows);

    if (g_HsDebugEnabled)
        LOG_INFO("server.loading",
            "[HearthsideChat] Loaded {} hside_grounded_template row(s) ({} skipped).", rows.size(), skipped);
}
