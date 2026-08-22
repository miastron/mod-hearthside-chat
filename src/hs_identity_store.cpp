#include "hs_identity_store.h"
#include "hs_archetype.h"
#include "hs_config.h"
#include "hs_identity.h"
#include "hs_json.h"
#include "hs_memory_store.h"

#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"

#include <algorithm>
#include <atomic>
#include <set>
#include <vector>

namespace
{
    std::atomic<uint32_t> g_PromotionsThisSession{0};
    std::atomic<uint32_t> g_DemotionsThisSession{0};
    std::atomic<uint32_t> g_RetirementsThisSession{0};

    // Idempotent append -- both vectors are re-applied on every startup and
    // `.reload config`, so a repeat call must not accumulate duplicate
    // entries.
    void AppendIfMissing(std::vector<std::string>& names, const std::string& name)
    {
        if (std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
    }

    void EraseIfPresent(std::vector<std::string>& names, const std::string& name)
    {
        names.erase(std::remove(names.begin(), names.end(), name), names.end());
    }

    std::string LookupBotName(uint64_t botGuid)
    {
        QueryResult result = CharacterDatabase.Query("SELECT name FROM characters WHERE guid = {}", botGuid);
        return result ? (*result)[0].Get<std::string>() : "";
    }

    void PushNameIntoExcludeVectors(const std::string& botName)
    {
        if (botName.empty())
            return;
        AppendIfMissing(sPlayerbotAIConfig.levelBracketsExcludeNames, botName);
        AppendIfMissing(sPlayerbotAIConfig.resetBotLevelExcludeNames, botName);
    }

    void RemoveNameFromExcludeVectors(const std::string& botName)
    {
        if (botName.empty())
            return;
        EraseIfPresent(sPlayerbotAIConfig.levelBracketsExcludeNames, botName);
        EraseIfPresent(sPlayerbotAIConfig.resetBotLevelExcludeNames, botName);
    }

    // Every hside_identity row is created exclusively by
    // Hs_BumpInteractionScore (always called with a bot's guid), so joining
    // against hside_identity is enough to guarantee these results are bots
    // -- no separate "is this guid a bot" check needed.
    std::set<uint64_t> FetchFriendedIdentityBotGuids()
    {
        std::set<uint64_t> guids;
        QueryResult result = CharacterDatabase.Query(
            "SELECT DISTINCT cs.friend FROM character_social cs "
            "JOIN hside_identity hi ON hi.bot_guid = cs.friend "
            "WHERE (cs.flags & 1) != 0");
        if (!result)
            return guids;

        do
        {
            guids.insert((*result)[0].Get<uint64_t>());
        } while (result->NextRow());

        return guids;
    }
}

void Hs_BumpInteractionScore(uint64_t botGuid, uint8_t botLevel, uint32_t weight)
{
    if (weight == 0)
        return;

    // A carded bot whose level drops is retired, not repaired. This call
    // site already has a trustworthy, freshly-read botLevel on every tier-2
    // delivery, so the level check runs before the score bump below -- a
    // level-dropped bot's next utterance retires it instead of scoring a
    // persona that no longer applies.
    QueryResult existing = CharacterDatabase.Query(
        "SELECT card_active, last_known_level FROM hside_identity WHERE bot_guid = {}", botGuid);
    if (existing && (*existing)[0].Get<bool>() && botLevel < (*existing)[1].Get<uint8_t>())
    {
        Hs_RetireCard(botGuid, botLevel);
        return;
    }

    HsArchetype archetype = Hs_ArchetypeForBot(botGuid, botLevel);
    std::string archetypeName = Hs_ArchetypeInfoFor(archetype).enumName;

    // Lazily create the row on first score event, otherwise just add to the
    // running total -- last_known_level/level_checked_at refresh on every
    // bump too, which is what makes the retirement check above trustworthy
    // on the next call.
    CharacterDatabase.Execute(
        "INSERT INTO hside_identity (bot_guid, archetype, last_known_level, level_checked_at, interaction_score, last_used_at) "
        "VALUES ({}, '{}', {}, NOW(), {}, NOW()) "
        "ON DUPLICATE KEY UPDATE interaction_score = interaction_score + {}, last_known_level = {}, level_checked_at = NOW(), last_used_at = NOW()",
        botGuid, archetypeName, static_cast<uint32_t>(botLevel), weight, weight, static_cast<uint32_t>(botLevel));

    QueryResult result = CharacterDatabase.Query(
        "SELECT interaction_score, promoted_at FROM hside_identity WHERE bot_guid = {}", botGuid);
    if (!result)
        return;

    uint32_t score      = (*result)[0].Get<uint32_t>();
    bool     alreadyPromoted = !(*result)[1].IsNull();

    if (!alreadyPromoted && score >= kHsPromotionThreshold)
    {
        CharacterDatabase.Execute(
            "UPDATE hside_identity SET promoted_at = NOW() WHERE bot_guid = {} AND promoted_at IS NULL", botGuid);
        g_PromotionsThisSession.fetch_add(1);
        if (g_HsDebugEnabled)
            LOG_INFO("server.loading", "[HearthsideChat] Bot {} promoted (score {}).", botGuid, score);
    }
}

HsCardSnapshot Hs_LookupCardSnapshot(uint64_t botGuid)
{
    HsCardSnapshot snapshot;

    QueryResult result = CharacterDatabase.Query(
        "SELECT card_voice, card_facts FROM hside_identity WHERE bot_guid = {} AND card_active = 1", botGuid);
    if (!result)
        return snapshot;

    snapshot.active     = true;
    snapshot.voiceBlock = (*result)[0].IsNull() ? "" : (*result)[0].Get<std::string>();

    if (!(*result)[1].IsNull())
    {
        std::string factsText = (*result)[1].Get<std::string>();
        hs_json facts = hs_json::parse(factsText, nullptr, /*allow_exceptions=*/false);
        if (!facts.is_discarded())
            snapshot.verbalTic = Hs_ExtractVerbalTic(facts);
    }

    return snapshot;
}

std::string Hs_LookupCardFactField(uint64_t botGuid, const std::string& fieldName)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT card_facts FROM hside_identity WHERE bot_guid = {} AND card_active = 1", botGuid);
    if (!result || (*result)[0].IsNull())
        return "";

    hs_json facts = hs_json::parse((*result)[0].Get<std::string>(), nullptr, /*allow_exceptions=*/false);
    if (facts.is_discarded())
        return "";

    return Hs_CardFactField(facts, fieldName);
}

bool Hs_HasActiveCard(uint64_t botGuid)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT 1 FROM hside_identity WHERE bot_guid = {} AND card_active = 1", botGuid);
    return result != nullptr;
}

void Hs_ApplyExcludeVectorsFromIdentityTable()
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT c.name FROM hside_identity hi JOIN characters c ON c.guid = hi.bot_guid WHERE hi.card_active = 1");
    if (!result)
        return;

    uint32_t count = 0;
    do
    {
        PushNameIntoExcludeVectors((*result)[0].Get<std::string>());
        ++count;
    } while (result->NextRow());

    if (g_HsDebugEnabled)
        LOG_INFO("server.loading", "[HearthsideChat] Re-applied {} carded bot name(s) to playerbots' recycling-exclusion vectors.", count);
}

void Hs_PushBotIntoExcludeVectors(uint64_t botGuid)
{
    PushNameIntoExcludeVectors(LookupBotName(botGuid));
}

void Hs_RemoveBotFromExcludeVectors(uint64_t botGuid)
{
    RemoveNameFromExcludeVectors(LookupBotName(botGuid));
}

void Hs_RetireCard(uint64_t botGuid, uint8_t newLevel)
{
    HsArchetype archetype = Hs_ArchetypeForBot(botGuid, newLevel);
    std::string archetypeName = Hs_ArchetypeInfoFor(archetype).enumName;

    CharacterDatabase.Execute(
        "UPDATE hside_identity SET card_voice = NULL, card_facts = NULL, card_model = NULL, "
        "card_prompt_version = NULL, card_active = 0, pinned_by_friend = 0, promoted_at = NULL, "
        "interaction_score = 0, archetype = '{}', last_known_level = {}, level_checked_at = NOW() "
        "WHERE bot_guid = {}",
        archetypeName, static_cast<uint32_t>(newLevel), botGuid);

    Hs_DropMemoryRowsForBot(botGuid);
    RemoveNameFromExcludeVectors(LookupBotName(botGuid));

    g_RetirementsThisSession.fetch_add(1);
    if (g_HsDebugEnabled)
        LOG_INFO("server.loading", "[HearthsideChat] Bot {} retired (level dropped to {}).", botGuid, newLevel);
}

void Hs_RunIdentityDailySweep()
{
    // 1. Friend poll -- promote/pin newly-friended rows, unpin rows no
    // longer found friended. See hs_identity_store.h's doc comment for why
    // this reads character_social directly instead of the live SocialMgr
    // API, and why it's scoped to existing hside_identity rows.
    std::set<uint64_t> friended = FetchFriendedIdentityBotGuids();

    QueryResult rows = CharacterDatabase.Query(
        "SELECT bot_guid, pinned_by_friend, promoted_at FROM hside_identity");
    if (rows)
    {
        do
        {
            uint64_t botGuid  = (*rows)[0].Get<uint64_t>();
            bool     pinned   = (*rows)[1].Get<bool>();
            bool     promoted = !(*rows)[2].IsNull();
            bool     isFriended = friended.count(botGuid) != 0;

            if (isFriended && !pinned)
            {
                CharacterDatabase.Execute("UPDATE hside_identity SET pinned_by_friend = 1 WHERE bot_guid = {}", botGuid);
                if (!promoted)
                {
                    CharacterDatabase.Execute(
                        "UPDATE hside_identity SET promoted_at = NOW() WHERE bot_guid = {} AND promoted_at IS NULL", botGuid);
                    g_PromotionsThisSession.fetch_add(1);
                    if (g_HsDebugEnabled)
                        LOG_INFO("server.loading", "[HearthsideChat] Bot {} promoted (friended).", botGuid);
                }
            }
            else if (!isFriended && pinned)
            {
                CharacterDatabase.Execute("UPDATE hside_identity SET pinned_by_friend = 0 WHERE bot_guid = {}", botGuid);
            }
        } while (rows->NextRow());
    }

    // 2. Score decay -- one point per day once a row has gone quiet for
    // kHsScoreDecayGraceDays. The sweep's own once-daily cadence is the
    // decay unit; no extra column is needed to track partial progress.
    CharacterDatabase.Execute(
        "UPDATE hside_identity SET interaction_score = GREATEST(0, interaction_score - {}) "
        "WHERE last_used_at IS NOT NULL AND last_used_at < NOW() - INTERVAL {} DAY AND interaction_score > 0",
        kHsScoreDecayPointsPerDay, kHsScoreDecayGraceDays);

    // 3. Card demotion -- dormant, unpinned cards clear. Card text is not
    // touched.
    QueryResult toDemote = CharacterDatabase.Query(
        "SELECT bot_guid FROM hside_identity WHERE card_active = 1 AND pinned_by_friend = 0 "
        "AND last_used_at < NOW() - INTERVAL {} DAY",
        kHsCardDormancyDays);
    if (toDemote)
    {
        do
        {
            uint64_t botGuid = (*toDemote)[0].Get<uint64_t>();
            CharacterDatabase.Execute("UPDATE hside_identity SET card_active = 0 WHERE bot_guid = {}", botGuid);
            RemoveNameFromExcludeVectors(LookupBotName(botGuid));
            g_DemotionsThisSession.fetch_add(1);
            if (g_HsDebugEnabled)
                LOG_INFO("server.loading", "[HearthsideChat] Bot {} demoted (dormant).", botGuid);
        } while (toDemote->NextRow());
    }

    // 4. Retirement for currently-online carded bots whose level dropped
    // while nobody was talking to them. Hs_BumpInteractionScore already
    // catches this live for bots still being chatted with; this covers the
    // bot-login / quiet-carded-bot gap. Only online bots can be checked here
    // (needs a live Player* for the current level) -- an offline one is
    // caught the next time it's talked to, or the next time this sweep
    // finds it online, same lazy-not-instant tolerance as the friend poll
    // above.
    QueryResult carded = CharacterDatabase.Query(
        "SELECT bot_guid, last_known_level FROM hside_identity WHERE card_active = 1");
    if (carded)
    {
        do
        {
            uint64_t botGuid     = (*carded)[0].Get<uint64_t>();
            uint8_t  storedLevel = (*carded)[1].Get<uint8_t>();
            Player*  bot         = ObjectAccessor::FindPlayer(ObjectGuid(botGuid));
            if (bot && bot->IsInWorld() && bot->GetLevel() < storedLevel)
                Hs_RetireCard(botGuid, bot->GetLevel());
        } while (carded->NextRow());
    }

    // 5. Orphan cleanup. AiPlayerbot.DeleteRandomBotAccounts wipes every
    // random-bot account/character in one shot, followed by an immediate
    // worldserver restart -- a one-shot startup action, not a live event
    // this module can hook. Any hside_identity/hside_memory row still
    // pointing at a since-deleted bot_guid is dead weight with no possible
    // owner to reconcile against: a full wipe invalidates every card
    // regardless of whether a recreated bot happens to reuse the old GUID.
    // Self-healing -- no operator step, no new config key -- and folded into
    // this existing daily sweep rather than a dedicated one, same reasoning
    // hs_main.cpp gives for putting decay/pinning/retirement here instead of
    // their own timer.
    QueryResult orphans = CharacterDatabase.Query(
        "SELECT hi.bot_guid FROM hside_identity hi "
        "LEFT JOIN characters c ON c.guid = hi.bot_guid WHERE c.guid IS NULL");
    if (orphans)
    {
        uint32_t orphanCount = 0;
        do
        {
            uint64_t orphanGuid = (*orphans)[0].Get<uint64_t>();
            Hs_DropMemoryRowsForBot(orphanGuid);
            CharacterDatabase.Execute("DELETE FROM hside_identity WHERE bot_guid = {}", orphanGuid);
            ++orphanCount;
        } while (orphans->NextRow());

        if (orphanCount > 0)
            LOG_INFO("server.loading",
                "[HearthsideChat] Cleaned up {} orphaned identity row(s) (bot no longer exists -- "
                "likely a DeleteRandomBotAccounts wipe).", orphanCount);
    }
}

uint32_t Hs_PromotionsThisSession()
{
    return g_PromotionsThisSession.load();
}

uint32_t Hs_DemotionsThisSession()
{
    return g_DemotionsThisSession.load();
}

uint32_t Hs_RetirementsThisSession()
{
    return g_RetirementsThisSession.load();
}

bool Hs_ForcePromote(uint64_t botGuid, uint8_t botLevel)
{
    HsArchetype archetype = Hs_ArchetypeForBot(botGuid, botLevel);
    std::string archetypeName = Hs_ArchetypeInfoFor(archetype).enumName;

    CharacterDatabase.Execute(
        "INSERT INTO hside_identity (bot_guid, archetype, last_known_level, level_checked_at, promoted_at) "
        "VALUES ({}, '{}', {}, NOW(), NOW()) "
        "ON DUPLICATE KEY UPDATE promoted_at = IFNULL(promoted_at, NOW())",
        botGuid, archetypeName, static_cast<uint32_t>(botLevel));

    QueryResult result = CharacterDatabase.Query(
        "SELECT promoted_at FROM hside_identity WHERE bot_guid = {}", botGuid);
    bool nowPromoted = result && !(*result)[0].IsNull();
    if (nowPromoted)
        g_PromotionsThisSession.fetch_add(1);
    return nowPromoted;
}

bool Hs_ForceDemote(uint64_t botGuid)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT card_active FROM hside_identity WHERE bot_guid = {}", botGuid);
    if (!result || !(*result)[0].Get<bool>())
        return false;

    CharacterDatabase.Execute("UPDATE hside_identity SET card_active = 0 WHERE bot_guid = {}", botGuid);
    RemoveNameFromExcludeVectors(LookupBotName(botGuid));
    g_DemotionsThisSession.fetch_add(1);
    return true;
}

HsIdentityInspection Hs_InspectIdentity(uint64_t botGuid)
{
    HsIdentityInspection out;

    QueryResult result = CharacterDatabase.Query(
        "SELECT archetype, last_known_level, interaction_score, promoted_at, card_active, "
        "pinned_by_friend, card_voice FROM hside_identity WHERE bot_guid = {}", botGuid);
    if (result)
    {
        out.hasIdentityRow   = true;
        out.archetype        = (*result)[0].Get<std::string>();
        out.lastKnownLevel   = (*result)[1].Get<uint8_t>();
        out.interactionScore = (*result)[2].Get<uint32_t>();
        out.promoted         = !(*result)[3].IsNull();
        out.cardActive       = (*result)[4].Get<bool>();
        out.pinnedByFriend   = (*result)[5].Get<bool>();
        if (out.cardActive && !(*result)[6].IsNull())
            out.voiceBlock = (*result)[6].Get<std::string>();
    }

    QueryResult memResult = CharacterDatabase.Query(
        "SELECT 1 FROM hside_memory WHERE bot_guid = {} LIMIT 1", botGuid);
    out.hasAnyMemoryRows = memResult != nullptr;

    return out;
}

void Hs_GmPinBot(uint64_t botGuid)
{
    PushNameIntoExcludeVectors(LookupBotName(botGuid));
}

void Hs_GmUnpinBot(uint64_t botGuid)
{
    RemoveNameFromExcludeVectors(LookupBotName(botGuid));
}

uint32_t Hs_IdentityRowCount()
{
    QueryResult result = CharacterDatabase.Query("SELECT COUNT(*) FROM hside_identity");
    return result ? (*result)[0].Get<uint32_t>() : 0;
}

uint32_t Hs_CardActiveCount()
{
    QueryResult result = CharacterDatabase.Query("SELECT COUNT(*) FROM hside_identity WHERE card_active = 1");
    return result ? (*result)[0].Get<uint32_t>() : 0;
}
