#include "hs_corpus.h"

#include "Bag.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameEventMgr.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QueryResult.h"
#include "QuestDef.h"
#include "Random.h"
#include "SharedDefines.h"

#include <sstream>
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

namespace
{
    // The exact hyperlink markup the core itself emits (see
    // PlayerStorage.cpp's access-requirement report), not a hand-rolled
    // approximation -- hs_style.cpp treats a full |c...|Hitem:...|h[...]|h|r
    // run as a protected token, and only matching markup gets that treatment.
    std::string BuildItemLink(ItemTemplate const* tmpl)
    {
        if (!tmpl)
            return "";

        std::ostringstream stream;
        stream << "|c" << std::hex << ItemQualityColors[tmpl->Quality] << std::dec
               << "|Hitem:" << tmpl->ItemId << ":0:0:0:0:0:0:0:0:0|h["
               << tmpl->Name1 << "]|h|r";
        return stream.str();
    }

    std::string BuildQuestLink(Quest const* quest)
    {
        if (!quest)
            return "";

        std::ostringstream stream;
        stream << "|cffff7c0a|Hquest:" << quest->GetQuestId() << ":" << quest->GetQuestLevel()
               << "|h[" << quest->GetTitle() << "]|h|r";
        return stream.str();
    }

    // Collects every non-soulbound item the bot is carrying, then picks one
    // at random -- picking the first found would make a bot's "WTS" line
    // repeat the same stack until the bag shifted.
    std::string RandomTradeableItemLink(Player* bot)
    {
        std::vector<ItemTemplate const*> carried;

        auto consider = [&carried](Item* item)
        {
            if (!item || item->IsSoulBound())
                return;
            if (ItemTemplate const* tmpl = item->GetTemplate())
                carried.push_back(tmpl);
        };

        // Backpack.
        for (uint8_t slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));

        // The four equipped bags.
        for (uint8_t bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* bag = bot->GetBagByPos(bagSlot);
            if (!bag)
                continue;
            for (uint32_t slot = 0; slot < bag->GetBagSize(); ++slot)
                consider(bag->GetItemByPos(static_cast<uint8_t>(slot)));
        }

        if (carried.empty())
            return "";

        return BuildItemLink(carried[urand(0, static_cast<uint32_t>(carried.size()) - 1)]);
    }

    std::string RandomActiveQuestLink(Player* bot)
    {
        std::vector<Quest const*> active;
        for (uint8_t slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32_t questId = bot->GetQuestSlotQuestId(slot);
            if (!questId)
                continue;
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
                active.push_back(quest);
        }

        if (active.empty())
            return "";

        return BuildQuestLink(active[urand(0, static_cast<uint32_t>(active.size()) - 1)]);
    }
}

std::string Hs_ClassNameFor(uint8_t classId)
{
    switch (classId)
    {
        case CLASS_WARRIOR:      return "warrior";
        case CLASS_PALADIN:      return "paladin";
        case CLASS_HUNTER:       return "hunter";
        case CLASS_ROGUE:        return "rogue";
        case CLASS_PRIEST:       return "priest";
        case CLASS_DEATH_KNIGHT: return "death knight";
        case CLASS_SHAMAN:       return "shaman";
        case CLASS_MAGE:         return "mage";
        case CLASS_WARLOCK:      return "warlock";
        case CLASS_DRUID:        return "druid";
        default:                 return "";
    }
}

HsPlaceholderContext Hs_BuildPlaceholderContext(Player* bot)
{
    HsPlaceholderContext ctx;
    if (!bot)
        return ctx;

    ctx.className = Hs_ClassNameFor(bot->getClass());
    ctx.level     = std::to_string(bot->GetLevel());

    if (AreaTableEntry const* entry = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        const char* name = entry->area_name[0];
        if (name && *name)
            ctx.zone = name;
    }

    if (uint32_t guildId = bot->GetGuildId())
    {
        if (Guild* guild = sGuildMgr->GetGuildById(guildId))
            ctx.guild = guild->GetName();
    }

    // Left empty when the bot has nothing to point at; Hs_ResolveUniversalPlaceholders
    // turns that into "drop the line" -- an empty-bagged bot must not advertise stock.
    ctx.itemLink  = RandomTradeableItemLink(bot);
    ctx.questLink = RandomActiveQuestLink(bot);

    return ctx;
}
