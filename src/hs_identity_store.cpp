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
#include <chrono>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace
{
    std::atomic<uint32_t> g_PromotionsThisSession{0};
    std::atomic<uint32_t> g_DemotionsThisSession{0};
    std::atomic<uint32_t> g_RetirementsThisSession{0};

    // Idempotent append: both vectors are re-applied on every startup and
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

    // ---- Exclude-vector writes are world-thread-only ----
    //
    // sPlayerbotAIConfig.levelBracketsExcludeNames/resetBotLevelExcludeNames
    // are plain std::vectors with no synchronization of their own, and
    // mod-playerbots walks them from the world thread (RandomBotLevelMgr's
    // IsNameInExcludeList sites). A mutex on this side alone would not close
    // the race, because that reader takes no lock, so the only correct fix
    // is to never touch the vectors off the world thread at all.
    //
    // The two Apply* functions below therefore write directly and are
    // world-thread-only; every other entry point (the generator finishing a
    // card, the queue worker retiring one, the HTTP control API's
    // pin/unpin/promote/demote routes) queues its intent here instead, and
    // Hs_DrainExcludeVectorQueue() applies it on the next world tick from
    // hs_main.cpp's HsIdentityLifecycleWorldScript::OnUpdate, the same
    // thread the existing 300s reconcile already writes them from. A push
    // and a remove for the same name stay in submission order, since the
    // queue is a FIFO vector.
    struct PendingExcludeOp
    {
        std::string name;
        bool        push; // false = remove
    };

    std::mutex                    g_PendingExcludeMutex;
    std::vector<PendingExcludeOp> g_PendingExcludeOps;

    // World-thread-only: writes sPlayerbotAIConfig's vectors directly.
    void ApplyPushNameIntoExcludeVectors(const std::string& botName)
    {
        if (botName.empty())
            return;
        AppendIfMissing(sPlayerbotAIConfig.levelBracketsExcludeNames, botName);
        AppendIfMissing(sPlayerbotAIConfig.resetBotLevelExcludeNames, botName);
    }

    // World-thread-only: writes sPlayerbotAIConfig's vectors directly.
    void ApplyRemoveNameFromExcludeVectors(const std::string& botName)
    {
        if (botName.empty())
            return;
        EraseIfPresent(sPlayerbotAIConfig.levelBracketsExcludeNames, botName);
        EraseIfPresent(sPlayerbotAIConfig.resetBotLevelExcludeNames, botName);
    }

    // Callable from any thread.
    void QueueExcludeVectorOp(const std::string& botName, bool push)
    {
        if (botName.empty())
            return;
        std::lock_guard<std::mutex> lock(g_PendingExcludeMutex);
        g_PendingExcludeOps.push_back(PendingExcludeOp{ botName, push });
    }

    void PushNameIntoExcludeVectors(const std::string& botName)
    {
        QueueExcludeVectorOp(botName, true);
    }

    void RemoveNameFromExcludeVectors(const std::string& botName)
    {
        QueueExcludeVectorOp(botName, false);
    }

    // Every hside_identity row is created exclusively by
    // Hs_BumpInteractionScore (always called with a bot's guid), so joining
    // against hside_identity is enough to guarantee these results are bots.
    // No separate "is this guid a bot" check needed.
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
    // delivery, so the level check runs before the score bump below: a
    // level-dropped bot's next utterance retires it instead of scoring a
    // persona that no longer applies.
    QueryResult existing = CharacterDatabase.Query(
        "SELECT card_active, last_known_level FROM hside_identity WHERE bot_guid = {}", botGuid);
    if (existing && (*existing)[0].Get<bool>() && botLevel < (*existing)[1].Get<uint8_t>())
    {
        Hs_RetireCard(botGuid, botLevel);
        return;
    }

    HsArchetype archetype = Hs_ArchetypeForBot(botGuid);
    std::string archetypeName = Hs_ArchetypeInfoFor(archetype).enumName;

    // Lazily create the row on first score event, otherwise just add to the
    // running total. last_known_level/level_checked_at refresh on every
    // bump too, which is what makes the retirement check above trustworthy
    // on the next call.
    //
    // Review A2: DirectExecute, not Execute. The promotion check immediately
    // below reads interaction_score back with Query(), which runs on the
    // synchronous connection pool while Execute() enqueues onto the async
    // worker -- the read would routinely observe the pre-bump value, so
    // promotion at kHsPromotionThreshold lagged by one qualifying
    // interaction and g_PromotionsThisSession could double-count when two
    // bumps landed between two reads. DirectExecute commits on the same pool
    // the read uses, so the read-after-write is ordered.
    CharacterDatabase.DirectExecute(
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
            LOG_INFO("module.hearthside", "[HearthsideChat] Bot {} promoted (score {}).", botGuid, score);
    }
}

// ---- card lookup cache (review G1) -------------------------------------
//
// Hs_LookupCardSnapshot is the single most repeated query in the module. It
// runs on the world thread inside the chat hook, once per tier tried per
// replying bot per message: TryReflex, TryGrounded, TryCorpusFallback and
// TryChannelCorpusReply each take a snapshot, and TryGrounded additionally
// called Hs_LookupCardFactField (another query) per fact-backed question
// kind. With CharacterDatabase.SynchThreads = 1 by default, every one of
// those blocks the world tick against the single synchronous connection,
// which the queue worker and generator are also using.
//
// The overwhelmingly common answer is "this bot has no active card", and a
// card changes only at four explicit moments: generation, demotion,
// retirement, and a GM/HTTP edit. So the row is cached per bot with a short
// TTL, and every writer invalidates explicitly -- the TTL is a backstop for
// a path that forgets to, not the primary mechanism.
//
// One cache entry serves both accessors: the raw card_facts text is kept so
// Hs_LookupCardFactField can answer any field without its own query.
namespace
{
    using CacheClock = std::chrono::steady_clock;

    // Short enough that an unnoticed direct DB edit corrects itself within a
    // minute, long enough that a burst of replies to one message costs one
    // query rather than six.
    constexpr int64_t kCardCacheTtlSeconds = 30;

    // Bounded so a long-running realm cannot accumulate an entry per bot
    // that ever spoke. Cleared wholesale rather than LRU-evicted: the
    // contents are reconstructible by definition, and this happens rarely.
    constexpr size_t  kCardCacheMaxEntries = 4096;

    struct CardCacheEntry
    {
        HsCardSnapshot          snapshot;
        std::string             factsText;  // raw card_facts, "" if NULL/absent
        CacheClock::time_point  cachedAt;
    };

    std::mutex                                     g_CardCacheMutex;
    std::unordered_map<uint64_t, CardCacheEntry>   g_CardCache;

    // Reads the row and refreshes the cache entry. Caller must not hold
    // g_CardCacheMutex: this blocks on the DB.
    CardCacheEntry FetchCardEntry(uint64_t botGuid)
    {
        CardCacheEntry entry;
        entry.cachedAt = CacheClock::now();

        QueryResult result = CharacterDatabase.Query(
            "SELECT card_voice, card_facts FROM hside_identity WHERE bot_guid = {} AND card_active = 1", botGuid);
        if (!result)
            return entry; // active stays false: no row, or the card is dormant/retired

        entry.snapshot.active     = true;
        entry.snapshot.voiceBlock = (*result)[0].IsNull() ? "" : (*result)[0].Get<std::string>();

        if (!(*result)[1].IsNull())
        {
            entry.factsText = (*result)[1].Get<std::string>();
            hs_json facts = hs_json::parse(entry.factsText, nullptr, /*allow_exceptions=*/false);
            if (!facts.is_discarded())
            {
                entry.snapshot.verbalTic = Hs_ExtractVerbalTic(facts);
                // Same already-parsed JSON the verbal tic comes from. Reading
                // these two here is what lets TryCorpusFallback resolve the
                // card-only placeholders without two more round trips for the
                // row it already has.
                entry.snapshot.mainFocus   = Hs_CardFactField(facts, "main_focus");
                entry.snapshot.currentGoal = Hs_CardFactField(facts, "current_goal");
            }
        }
        return entry;
    }

    CardCacheEntry CardEntryFor(uint64_t botGuid)
    {
        {
            std::lock_guard<std::mutex> lock(g_CardCacheMutex);
            auto it = g_CardCache.find(botGuid);
            if (it != g_CardCache.end())
            {
                auto age = std::chrono::duration_cast<std::chrono::seconds>(
                    CacheClock::now() - it->second.cachedAt).count();
                if (age < kCardCacheTtlSeconds)
                    return it->second;
            }
        }

        CardCacheEntry entry = FetchCardEntry(botGuid);

        std::lock_guard<std::mutex> lock(g_CardCacheMutex);
        if (g_CardCache.size() >= kCardCacheMaxEntries)
            g_CardCache.clear();
        g_CardCache[botGuid] = entry;
        return entry;
    }
}

void Hs_InvalidateCardCache(uint64_t botGuid)
{
    std::lock_guard<std::mutex> lock(g_CardCacheMutex);
    g_CardCache.erase(botGuid);
}

void Hs_InvalidateAllCardCache()
{
    std::lock_guard<std::mutex> lock(g_CardCacheMutex);
    g_CardCache.clear();
}

HsCardSnapshot Hs_LookupCardSnapshot(uint64_t botGuid)
{
    return CardEntryFor(botGuid).snapshot;
}

std::string Hs_LookupCardFactField(uint64_t botGuid, const std::string& fieldName)
{
    // Review G1: served from the same cached row as the snapshot, so the
    // three fact-backed grounded answers (current_goal, played_since, alt)
    // no longer cost a query each on top of the snapshot the caller has
    // already taken.
    CardCacheEntry entry = CardEntryFor(botGuid);
    if (!entry.snapshot.active || entry.factsText.empty())
        return "";

    hs_json facts = hs_json::parse(entry.factsText, nullptr, /*allow_exceptions=*/false);
    if (facts.is_discarded())
        return "";

    return Hs_CardFactField(facts, fieldName);
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
        // Direct, not queued: this function is world-thread-only (startup,
        // `.reload config`, and the 300s reconcile, all from hs_main.cpp).
        ApplyPushNameIntoExcludeVectors((*result)[0].Get<std::string>());
        ++count;
    } while (result->NextRow());

    if (g_HsDebugEnabled)
        LOG_INFO("module.hearthside", "[HearthsideChat] Re-applied {} carded bot name(s) to playerbots' recycling-exclusion vectors.", count);
}

void Hs_DrainExcludeVectorQueue()
{
    std::vector<PendingExcludeOp> ops;
    {
        std::lock_guard<std::mutex> lock(g_PendingExcludeMutex);
        if (g_PendingExcludeOps.empty())
            return;
        ops.swap(g_PendingExcludeOps);
    }

    // Applied in submission order, so a push followed by a remove for the
    // same name lands the same way it would have if each had been applied
    // inline.
    for (auto const& op : ops)
    {
        if (op.push)
            ApplyPushNameIntoExcludeVectors(op.name);
        else
            ApplyRemoveNameFromExcludeVectors(op.name);
    }
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
    HsArchetype archetype = Hs_ArchetypeForBot(botGuid);
    std::string archetypeName = Hs_ArchetypeInfoFor(archetype).enumName;

    CharacterDatabase.Execute(
        "UPDATE hside_identity SET card_voice = NULL, card_facts = NULL, card_model = NULL, "
        "card_prompt_version = NULL, card_active = 0, pinned_by_friend = 0, promoted_at = NULL, "
        "interaction_score = 0, archetype = '{}', last_known_level = {}, level_checked_at = NOW() "
        "WHERE bot_guid = {}",
        archetypeName, static_cast<uint32_t>(newLevel), botGuid);

    Hs_InvalidateCardCache(botGuid); // review G1: the card is gone as of now
    Hs_DropMemoryRowsForBot(botGuid);
    RemoveNameFromExcludeVectors(LookupBotName(botGuid));

    g_RetirementsThisSession.fetch_add(1);
    if (g_HsDebugEnabled)
        LOG_INFO("module.hearthside", "[HearthsideChat] Bot {} retired (level dropped to {}).", botGuid, newLevel);
}

void Hs_RunIdentityDailySweep()
{
    // 1. Friend poll: promote/pin newly-friended rows, unpin rows no
    // longer found friended. See hs_identity_store.h's doc comment for why
    // this reads character_social directly instead of the live SocialMgr
    // API, and why it's scoped to existing hside_identity rows.
    std::set<uint64_t> friended = FetchFriendedIdentityBotGuids();

    QueryResult rows = CharacterDatabase.Query(
        "SELECT bot_guid, pinned_by_friend, promoted_at FROM hside_identity");
    if (rows)
    {
        // Review G8: collect the guids that need each transition, then issue
        // one statement per transition instead of one UPDATE per identity
        // row. The old loop sent up to three statements per row over the
        // whole table -- on a realm with a few thousand identity rows that
        // is thousands of round trips for a sweep whose actual effect is
        // usually a handful of flips.
        std::vector<uint64_t> toPin;
        std::vector<uint64_t> toUnpin;
        std::vector<uint64_t> toPromote;

        do
        {
            uint64_t botGuid  = (*rows)[0].Get<uint64_t>();
            bool     pinned   = (*rows)[1].Get<bool>();
            bool     promoted = !(*rows)[2].IsNull();
            bool     isFriended = friended.count(botGuid) != 0;

            if (isFriended && !pinned)
            {
                toPin.push_back(botGuid);
                if (!promoted)
                    toPromote.push_back(botGuid);
            }
            else if (!isFriended && pinned)
            {
                toUnpin.push_back(botGuid);
            }
        } while (rows->NextRow());

        // Comma-joined guid list for an IN (...) clause. Values are
        // uint64_t read back out of our own table, so no escaping applies.
        auto guidList = [](const std::vector<uint64_t>& guids)
        {
            std::string out;
            for (uint64_t guid : guids)
            {
                if (!out.empty())
                    out += ",";
                out += std::to_string(guid);
            }
            return out;
        };

        if (!toPin.empty())
            CharacterDatabase.Execute(
                "UPDATE hside_identity SET pinned_by_friend = 1 WHERE bot_guid IN ({})", guidList(toPin));
        if (!toUnpin.empty())
            CharacterDatabase.Execute(
                "UPDATE hside_identity SET pinned_by_friend = 0 WHERE bot_guid IN ({})", guidList(toUnpin));
        if (!toPromote.empty())
        {
            // promoted_at IS NULL is kept in the predicate, exactly as the
            // per-row version had it: the read above and this write are on
            // different connections, so the guard is what makes a
            // concurrent promotion (Hs_BumpInteractionScore) idempotent
            // rather than a second promotion.
            CharacterDatabase.Execute(
                "UPDATE hside_identity SET promoted_at = NOW() WHERE promoted_at IS NULL AND bot_guid IN ({})",
                guidList(toPromote));
            g_PromotionsThisSession.fetch_add(static_cast<uint32_t>(toPromote.size()));
            if (g_HsDebugEnabled)
                LOG_INFO("module.hearthside", "[HearthsideChat] {} bot(s) promoted (friended).", toPromote.size());
        }
    }

    // 2. Score decay: one point per day once a row has gone quiet for
    // kHsScoreDecayGraceDays. The sweep's own once-daily cadence is the
    // decay unit; no extra column is needed to track partial progress.
    CharacterDatabase.Execute(
        "UPDATE hside_identity SET interaction_score = GREATEST(0, interaction_score - {}) "
        "WHERE last_used_at IS NOT NULL AND last_used_at < NOW() - INTERVAL {} DAY AND interaction_score > 0",
        kHsScoreDecayPointsPerDay, kHsScoreDecayGraceDays);

    // 3. Card demotion: dormant, unpinned cards clear. Card text is not
    // touched.
    //
    // COALESCE, not a bare last_used_at: that column is NULL until
    // Hs_BumpInteractionScore first writes it, and in MySQL `NULL < <expr>`
    // is NULL, so a bare comparison silently never selects such a row. That
    // state is ordinary, not exotic: `.hearthside promote` (Hs_ForcePromote)
    // inserts without last_used_at, the generator then flips card_active = 1
    // without touching it either, and the friend poll in step 1 above
    // promotes the same shape. Without the COALESCE a GM-promoted bot that
    // is never actually talked to keeps its card, and its exclude-vector pin
    // against playerbots' recycler, forever. promoted_at is the right
    // fallback reference point since the row has no creation timestamp,
    // same trick Hs_RunUnusedRowEvictionSweep uses for corpus rows.
    QueryResult toDemote = CharacterDatabase.Query(
        "SELECT bot_guid FROM hside_identity WHERE card_active = 1 AND pinned_by_friend = 0 "
        "AND COALESCE(last_used_at, promoted_at) < NOW() - INTERVAL {} DAY",
        kHsCardDormancyDays);
    if (toDemote)
    {
        do
        {
            uint64_t botGuid = (*toDemote)[0].Get<uint64_t>();
            CharacterDatabase.Execute("UPDATE hside_identity SET card_active = 0 WHERE bot_guid = {}", botGuid);
            Hs_InvalidateCardCache(botGuid); // review G1
            RemoveNameFromExcludeVectors(LookupBotName(botGuid));
            g_DemotionsThisSession.fetch_add(1);
            if (g_HsDebugEnabled)
                LOG_INFO("module.hearthside", "[HearthsideChat] Bot {} demoted (dormant).", botGuid);
        } while (toDemote->NextRow());
    }

    // 4. Retirement for carded bots whose level dropped while nobody was
    // talking to them. Hs_BumpInteractionScore already catches this live for
    // bots still being chatted with; this covers the quiet-carded-bot gap.
    // Joined against `characters.level` (persisted on save/logout) so an
    // offline bot is covered too; a live Player* is still preferred when
    // available since it reflects a level-up not yet saved to the DB.
    QueryResult carded = CharacterDatabase.Query(
        "SELECT hi.bot_guid, hi.last_known_level, c.level FROM hside_identity hi "
        "JOIN characters c ON c.guid = hi.bot_guid WHERE hi.card_active = 1");
    if (carded)
    {
        do
        {
            uint64_t botGuid      = (*carded)[0].Get<uint64_t>();
            uint8_t  storedLevel  = (*carded)[1].Get<uint8_t>();
            uint8_t  persistedLevel = (*carded)[2].Get<uint8_t>();
            Player*  bot          = ObjectAccessor::FindPlayer(ObjectGuid(botGuid));
            uint8_t  currentLevel = (bot && bot->IsInWorld()) ? bot->GetLevel() : persistedLevel;
            if (currentLevel < storedLevel)
                Hs_RetireCard(botGuid, currentLevel);
        } while (carded->NextRow());
    }

    // 5. Orphan cleanup. AiPlayerbot.DeleteRandomBotAccounts wipes every
    // random-bot account/character in one shot, followed by an immediate
    // worldserver restart: a one-shot startup action, not a live event
    // this module can hook. Any hside_identity/hside_memory row still
    // pointing at a since-deleted bot_guid is dead weight with no possible
    // owner to reconcile against: a full wipe invalidates every card
    // regardless of whether a recreated bot happens to reuse the old GUID.
    // Self-healing (no operator step, no new config key) and folded into
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
            LOG_INFO("module.hearthside",
                "[HearthsideChat] Cleaned up {} orphaned identity row(s) (bot no longer exists -- "
                "likely a DeleteRandomBotAccounts wipe).", orphanCount);
    }

    // Review G1: this sweep demotes, retires and deletes rows in bulk, so
    // rather than tracking every guid each pass touched, drop the whole
    // card cache once at the end. It runs daily and rebuilds lazily.
    Hs_InvalidateAllCardCache();
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
    HsArchetype archetype = Hs_ArchetypeForBot(botGuid);
    std::string archetypeName = Hs_ArchetypeInfoFor(archetype).enumName;

    // Read the prior state *before* the upsert. Reading it afterwards cannot
    // distinguish the two cases: the ON DUPLICATE KEY clause leaves
    // promoted_at non-null either way, so an already-promoted bot would
    // report a fresh promotion and re-increment the session counter.
    QueryResult before = CharacterDatabase.Query(
        "SELECT promoted_at FROM hside_identity WHERE bot_guid = {}", botGuid);
    if (before && !(*before)[0].IsNull())
        return false; // already promoted, nothing to write, nothing to count

    // The ON DUPLICATE KEY clause still matters: the row may exist and be
    // unpromoted, which is exactly the case that reaches here.
    CharacterDatabase.Execute(
        "INSERT INTO hside_identity (bot_guid, archetype, last_known_level, level_checked_at, promoted_at) "
        "VALUES ({}, '{}', {}, NOW(), NOW()) "
        "ON DUPLICATE KEY UPDATE promoted_at = IFNULL(promoted_at, NOW())",
        botGuid, archetypeName, static_cast<uint32_t>(botLevel));

    g_PromotionsThisSession.fetch_add(1);
    return true;
}

bool Hs_ForceDemote(uint64_t botGuid)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT card_active FROM hside_identity WHERE bot_guid = {}", botGuid);
    if (!result || !(*result)[0].Get<bool>())
        return false;

    CharacterDatabase.Execute("UPDATE hside_identity SET card_active = 0 WHERE bot_guid = {}", botGuid);
    Hs_InvalidateCardCache(botGuid); // review G1
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
    // Review C18: actually the "thin wrapper" the header describes.
    // Hs_RemoveBotFromExcludeVectors was publicly declared but had no caller
    // anywhere, because both GM entry points reached past it to the
    // file-local helper -- so the exported half of a documented pair was
    // dead code while its duplicate was the one in use.
    Hs_PushBotIntoExcludeVectors(botGuid);
}

void Hs_GmUnpinBot(uint64_t botGuid)
{
    Hs_RemoveBotFromExcludeVectors(botGuid);
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
