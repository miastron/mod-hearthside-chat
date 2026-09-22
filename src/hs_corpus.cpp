#include "hs_corpus.h"
#include "hs_class.h"
#include "hs_locale.h"
#include "hs_log.h"
#include "hs_text.h"

#include "Bag.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameEventMgr.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QueryResult.h"
#include "QuestDef.h"
#include "Random.h"
#include "SharedDefines.h"

#include <functional>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace
{
    std::mutex                                           g_CategoryMutex;
    std::shared_ptr<const std::vector<HsCorpusCategory>> g_Categories =
        std::make_shared<const std::vector<HsCorpusCategory>>();
}

void Hs_LoadCorpusCategoriesFromDb()
{
    auto loaded = std::make_shared<std::vector<HsCorpusCategory>>();

    QueryResult result = CharacterDatabase.Query(
        "SELECT name, tag_axis, channel, card_gated, is_opener FROM hside_corpus_category");
    if (result)
    {
        do
        {
            HsCorpusCategory category;
            category.name      = (*result)[0].Get<std::string>();
            category.tagAxis   = (*result)[1].Get<std::string>();
            category.channel   = (*result)[2].IsNull() ? "" : (*result)[2].Get<std::string>();
            category.cardGated = (*result)[3].Get<uint8_t>() != 0;
            category.isOpener  = (*result)[4].Get<uint8_t>() != 0;
            loaded->push_back(std::move(category));
        } while (result->NextRow());
    }

    size_t count = loaded->size();
    {
        std::lock_guard<std::mutex> lock(g_CategoryMutex);
        g_Categories = std::move(loaded);
    }
    LOG_INFO(kHsLog, "[HearthsideChat] Loaded {} corpus categories.", count);
}

std::shared_ptr<const std::vector<HsCorpusCategory>> Hs_CorpusCategories()
{
    std::lock_guard<std::mutex> lock(g_CategoryMutex);
    return g_Categories;
}

bool Hs_FindCorpusCategory(const std::string& name, HsCorpusCategory& out)
{
    std::string wanted = HsText::Hs_ToLowerAscii(name);
    for (HsCorpusCategory const& category : *Hs_CorpusCategories())
    {
        if (HsText::Hs_ToLowerAscii(category.name) == wanted)
        {
            out = category;
            return true;
        }
    }
    return false;
}

namespace
{
    // Weighted random that penalizes recently-used rows: rather than always
    // taking the single least-exposed row (which would make a small pool
    // perfectly predictable), the SQL side orders candidates by
    // exposure/recency and this caps how many of the freshest rows are in
    // play; the C++ side then picks uniformly among them.
    constexpr int kAntiRepeatPoolSize = 5;

    // Builds the WHERE-clause fragment that narrows hside_corpus rows to
    // this bot's tag value for a category's axis.
    bool TagWhereFor(const std::string& axis, uint8_t botClass, const std::string& band,
                      uint8_t botFaction, uint32_t botZoneId, std::string& out)
    {
        if (axis == "none")       { out = "";                                                return true; }
        if (axis == "class")      { out = "AND class_tag = " + std::to_string(botClass);      return true; }
        if (axis == "level_band") { out = "AND level_band_tag = '" + band + "'";               return true; }
        if (axis == "faction")    { out = "AND faction_tag = " + std::to_string(botFaction);   return true; }
        if (axis == "zone")       { out = "AND zone_tag = " + std::to_string(botZoneId);        return true; }
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
        // Review D1: categoryName arrives from hside_corpus_category.name,
        // which `.hearthside capture <bot> <category>` can populate from GM
        // console input. hs_generator.cpp escapes it on every equivalent
        // statement; this one did not. tagWhere is module-built (a fixed
        // column name plus an integer or a fixed band label) and needs none.
        std::string escapedCategory = categoryName;
        CharacterDatabase.EscapeString(escapedCategory);

        QueryResult rowResult = CharacterDatabase.Query(
            "SELECT id, text FROM hside_corpus WHERE name = '{}' {} {} "
            "ORDER BY times_used ASC, last_used_at IS NULL DESC, last_used_at ASC LIMIT {}",
            escapedCategory, tagWhere, EventDormancyWhere(), kAntiRepeatPoolSize);
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

    // Category set -> one line. The three public selectors below differ only
    // in which categories they consider; everything after that -- resolve
    // each category's tag axis against this bot, drop the ones it does not
    // qualify for, pick one at random, then anti-repeat-pick a row within it
    // -- was copy-pasted three times in this file (review item 20).
    std::string SelectFromCategorySet(const std::function<bool(const HsCorpusCategory&)>& inSet, uint8_t botClass,
                                       uint8_t botLevel, uint8_t botFaction, uint32_t botZoneId)
    {
        std::string band = Hs_LevelBandFor(botLevel);

        // Each entry: category name, and the extra WHERE-clause fragment (if
        // any) narrowing hside_corpus rows to this bot's tag value for that
        // category's axis.
        std::vector<std::pair<std::string, std::string>> eligible;
        for (HsCorpusCategory const& category : *Hs_CorpusCategories())
        {
            if (!inSet(category))
                continue;

            std::string tagWhere;
            if (TagWhereFor(category.tagAxis, botClass, band, botFaction, botZoneId, tagWhere))
                eligible.emplace_back(category.name, tagWhere);
        }

        if (eligible.empty())
            return "";

        auto const& chosen = eligible[urand(0, static_cast<uint32_t>(eligible.size() - 1))];
        return PickAntiRepeatRow(chosen.first, chosen.second);
    }
}

std::string Hs_SelectCorpusLine(uint8_t botClass, uint8_t botLevel, uint8_t botFaction, uint32_t botZoneId, bool hasActiveCard)
{
    // The /say and direct-reply set: channel-less, non-opener categories,
    // with the card-gated ones admitted only for a bot that has a card.
    return SelectFromCategorySet(
        [hasActiveCard](const HsCorpusCategory& c)
        {
            return c.channel.empty() && !c.isOpener && (!c.cardGated || hasActiveCard);
        },
        botClass, botLevel, botFaction, botZoneId);
}

std::string Hs_SelectOpenerLine(const std::string& categoryName, uint8_t botClass, uint8_t botLevel,
                                 uint8_t botFaction, uint32_t botZoneId)
{
    // card_gated categories are unconditionally excluded here. No
    // is_opener=1 category is card_gated yet, so this is a defensive floor
    // rather than plumbing for a real signal.
    HsCorpusCategory category;
    if (!Hs_FindCorpusCategory(categoryName, category) || !category.isOpener || category.cardGated)
        return ""; // category missing, or not flagged as an opener category

    std::string band = Hs_LevelBandFor(botLevel);

    std::string tagWhere;
    if (!TagWhereFor(category.tagAxis, botClass, band, botFaction, botZoneId, tagWhere))
        return "";

    return PickAntiRepeatRow(category.name, tagWhere);
}

std::string Hs_SelectChannelLine(HsChannelKind kind, uint8_t botClass, uint8_t botLevel,
                                  uint8_t botFaction, uint32_t botZoneId)
{
    // Only Trade/General have channel_* categories seeded; any other kind
    // simply matches none and returns empty, like any "nothing eligible".
    std::string channelColumn = Hs_ChannelColumnName(kind);
    return SelectFromCategorySet(
        [&channelColumn](const HsCorpusCategory& c)
        {
            return c.channel == channelColumn && !c.isOpener && !c.cardGated;
        },
        botClass, botLevel, botFaction, botZoneId);
}

std::string Hs_SelectGroupAmbientLine(bool isRaid, uint8_t botClass, uint8_t botLevel,
                                       uint8_t botFaction, uint32_t botZoneId)
{
    // Same category set as Hs_SelectChannelLine, scoped by a different
    // `channel` value: party/raid aren't an HsChannelKind (see hs_corpus.h
    // for why they aren't).
    std::string channelColumn = isRaid ? "raid" : "party";
    return SelectFromCategorySet(
        [&channelColumn](const HsCorpusCategory& c)
        {
            return c.channel == channelColumn && !c.isOpener && !c.cardGated;
        },
        botClass, botLevel, botFaction, botZoneId);
}

namespace
{
    // The exact hyperlink markup the core itself emits (see
    // PlayerStorage.cpp's access-requirement report), not a hand-rolled
    // approximation. hs_style.cpp treats a full |c...|Hitem:...|h[...]|h|r
    // run as a protected token, and only matching markup gets that treatment.
    std::string BuildItemLink(ItemTemplate const* tmpl)
    {
        if (!tmpl)
            return "";

        // Review item 8: an empty display label is checked, not just a null
        // template. Hs_LocalizedItemName returns "" when the name resolves
        // empty as well as on null input, and "|h[]|h|r" is a hyperlink the
        // client renders as an empty bracket pair. "" is what both callers
        // already expect from the null branch, so this degrades to "no link
        // available" rather than to malformed markup.
        std::string name = Hs_LocalizedItemName(tmpl); // review H1
        if (name.empty())
            return "";

        std::ostringstream stream;
        stream << "|c" << std::hex << ItemQualityColors[tmpl->Quality] << std::dec
               << "|Hitem:" << tmpl->ItemId << ":0:0:0:0:0:0:0:0:0|h["
               << name << "]|h|r";
        return stream.str();
    }

    std::string BuildQuestLink(Quest const* quest)
    {
        if (!quest)
            return "";

        // Review item 7: Hs_LocalizedQuestTitle, not Quest::GetTitle. The
        // raw accessor returns the enUS row, so on a non-enUS realm a
        // %quest_link would show English next to a %item_link that correctly
        // showed the realm locale -- the exact mismatch the H1 sweep closed
        // for BuildItemLink seven lines above, which this function never got.
        // Empty-guarded for the same reason as BuildItemLink (item 8).
        std::string title = Hs_LocalizedQuestTitle(quest);
        if (title.empty())
            return "";

        std::ostringstream stream;
        stream << "|cffff7c0a|Hquest:" << quest->GetQuestId() << ":" << quest->GetQuestLevel()
               << "|h[" << title << "]|h|r";
        return stream.str();
    }

    // Collects every non-soulbound item the bot is carrying, then picks one
    // at random. Picking the first found would make a bot's "WTS" line
    // repeat the same stack until the bag shifted.
    //
    // Collects Item* rather than ItemTemplate const* so the stack size
    // survives: %item_link never needed it, but the TradePrice quote does
    // (mod-playerbots prices a stack, not a unit). Both consumers share this
    // one walk so a quote and a WTS line can never be drawing from different
    // pools by different rules.
    Item* PickTradeableItem(Player* bot)
    {
        std::vector<Item*> carried;

        auto consider = [&carried](Item* item)
        {
            if (!item || item->IsSoulBound())
                return;
            if (item->GetTemplate())
                carried.push_back(item);
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
            return nullptr;

        return carried[urand(0, static_cast<uint32_t>(carried.size()) - 1)];
    }

    std::string RandomTradeableItemLink(Player* bot)
    {
        Item* item = PickTradeableItem(bot);
        return item ? BuildItemLink(item->GetTemplate()) : "";
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

bool Hs_PickTradeableItem(Player* bot, HsTradeableItem& out)
{
    if (!bot)
        return false;

    Item* item = PickTradeableItem(bot);
    if (!item)
        return false;

    ItemTemplate const* tmpl = item->GetTemplate();
    if (!tmpl)
        return false; // defensive, PickTradeableItem already required one

    out.itemId = tmpl->ItemId;
    out.count  = item->GetCount();
    out.link    = BuildItemLink(tmpl);
    return true;
}

std::string Hs_ClassNameFor(uint8_t classId)
{
    // The words come from hs_class.h so hs_identity.cpp's card-fact
    // validation checks against the same spellings (review item 19); the
    // id mapping stays here, where the core's CLASS_* enum is in scope.
    switch (classId)
    {
        case CLASS_WARRIOR:      return HsClass::kNames[HsClass::Warrior];
        case CLASS_PALADIN:      return HsClass::kNames[HsClass::Paladin];
        case CLASS_HUNTER:       return HsClass::kNames[HsClass::Hunter];
        case CLASS_ROGUE:        return HsClass::kNames[HsClass::Rogue];
        case CLASS_PRIEST:       return HsClass::kNames[HsClass::Priest];
        case CLASS_DEATH_KNIGHT: return HsClass::kNames[HsClass::DeathKnight];
        case CLASS_SHAMAN:       return HsClass::kNames[HsClass::Shaman];
        case CLASS_MAGE:         return HsClass::kNames[HsClass::Mage];
        case CLASS_WARLOCK:      return HsClass::kNames[HsClass::Warlock];
        case CLASS_DRUID:        return HsClass::kNames[HsClass::Druid];
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
        std::string name = Hs_LocalizedAreaName(entry); // review H1
        if (!name.empty())
            ctx.zone = name;
    }

    if (uint32_t guildId = bot->GetGuildId())
    {
        if (Guild* guild = sGuildMgr->GetGuildById(guildId))
            ctx.guild = guild->GetName();
    }

    // Left empty when the bot has nothing to point at; Hs_ResolveUniversalPlaceholders
    // turns that into "drop the line": an empty-bagged bot must not advertise stock.
    ctx.itemLink  = RandomTradeableItemLink(bot);
    ctx.questLink = RandomActiveQuestLink(bot);

    return ctx;
}
