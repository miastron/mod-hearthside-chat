#include "hs_corpus.h"

#include "DatabaseEnv.h"
#include "GameEventMgr.h"
#include "QueryResult.h"
#include "Random.h"

#include <utility>
#include <vector>

namespace
{
    // Weighted random that penalizes recently-used rows: rather than always
    // taking the single least-exposed row (which would make a small pool
    // perfectly predictable), the SQL side orders candidates by
    // exposure/recency and this caps how many of the freshest rows are in
    // play; the C++ side then picks uniformly among them.
    constexpr int kAntiRepeatPoolSize = 5;

    // Builds the WHERE-clause fragment that narrows hside_corpus rows to
    // this bot's tag value for a category's axis. Returns false for
    // faction/zone -- no seeded category uses either yet, and this fallback
    // has no faction/zone signal wired through.
    bool TagWhereFor(const std::string& axis, uint8_t botClass, const std::string& band, std::string& out)
    {
        if (axis == "none")       { out = "";                                                return true; }
        if (axis == "class")      { out = "AND class_tag = " + std::to_string(botClass);      return true; }
        if (axis == "level_band") { out = "AND level_band_tag = '" + band + "'";               return true; }
        return false;
    }

    // Seasonal rows go dormant, not evicted: a row with a non-null event_id
    // is only selectable while its game event is active. Built as a WHERE
    // fragment (rather than filtering the C++ pool after the fact) so the
    // anti-repeat pool below is still filled from eligible rows only.
    // sGameEventMgr's active-event set is small and in-memory, so this reads
    // it once per selection rather than querying the DB for it.
    std::string EventDormancyWhere()
    {
        GameEventMgr::ActiveEvents const& active = sGameEventMgr->GetActiveEventList();
        if (active.empty())
            return "AND event_id IS NULL";

        std::string ids;
        for (uint16 id : active)
        {
            if (!ids.empty())
                ids += ",";
            ids += std::to_string(id);
        }
        return "AND (event_id IS NULL OR event_id IN (" + ids + "))";
    }

    // Shared anti-repeat pick + exposure bookkeeping, used by both
    // Hs_SelectCorpusLine and Hs_SelectOpenerLine below.
    std::string PickAntiRepeatRow(const std::string& categoryName, const std::string& tagWhere)
    {
        QueryResult rowResult = CharacterDatabase.Query(
            "SELECT id, text FROM hside_corpus WHERE name = '{}' {} {} "
            "ORDER BY times_used ASC, last_used_at IS NULL DESC, last_used_at ASC LIMIT {}",
            categoryName, tagWhere, EventDormancyWhere(), kAntiRepeatPoolSize);
        if (!rowResult)
            return "";

        std::vector<std::pair<uint32_t, std::string>> pool;
        do
        {
            pool.emplace_back((*rowResult)[0].Get<uint32_t>(), (*rowResult)[1].Get<std::string>());
        } while (rowResult->NextRow());

        auto const& picked = pool[urand(0, static_cast<uint32_t>(pool.size() - 1))];

        // Fire-and-forget: exposure bookkeeping, not on the critical path for
        // the reply already returned below.
        CharacterDatabase.Execute(
            "UPDATE hside_corpus SET times_used = times_used + 1, last_used_at = NOW() WHERE id = {}",
            picked.first);

        return picked.second;
    }
}

std::string Hs_SelectCorpusLine(uint8_t botClass, uint8_t botLevel, bool hasActiveCard)
{
    QueryResult catResult = CharacterDatabase.Query(
        "SELECT name, tag_axis FROM hside_corpus_category WHERE channel IS NULL AND is_opener = 0 AND (card_gated = 0 OR {})",
        hasActiveCard ? 1 : 0);
    if (!catResult)
        return "";

    std::string band = Hs_LevelBandFor(botLevel);

    // name, and the extra WHERE-clause fragment (if any) that narrows
    // hside_corpus rows to this bot's tag value for that category's axis.
    std::vector<std::pair<std::string, std::string>> eligible;
    do
    {
        std::string name = (*catResult)[0].Get<std::string>();
        std::string axis = (*catResult)[1].Get<std::string>();

        std::string tagWhere;
        if (TagWhereFor(axis, botClass, band, tagWhere))
            eligible.emplace_back(name, tagWhere);
        // faction/zone: skipped, see TagWhereFor.
    } while (catResult->NextRow());

    if (eligible.empty())
        return "";

    auto const& chosen = eligible[urand(0, static_cast<uint32_t>(eligible.size() - 1))];
    return PickAntiRepeatRow(chosen.first, chosen.second);
}

std::string Hs_SelectOpenerLine(const std::string& categoryName, uint8_t botClass, uint8_t botLevel)
{
    // card_gated categories are unconditionally excluded here -- no
    // is_opener=1 category is card_gated yet, so this is a defensive floor
    // rather than plumbing for a real signal, same as the faction/zone skip
    // below.
    QueryResult catResult = CharacterDatabase.Query(
        "SELECT tag_axis FROM hside_corpus_category WHERE name = '{}' AND is_opener = 1 AND card_gated = 0", categoryName);
    if (!catResult)
        return ""; // category missing, or not flagged as an opener category

    std::string axis = (*catResult)[0].Get<std::string>();
    std::string band = Hs_LevelBandFor(botLevel);

    std::string tagWhere;
    if (!TagWhereFor(axis, botClass, band, tagWhere))
        return ""; // faction/zone: not supported yet, same scoping as Hs_SelectCorpusLine

    return PickAntiRepeatRow(categoryName, tagWhere);
}
