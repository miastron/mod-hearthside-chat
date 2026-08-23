#include "hs_opener.h"
#include "hs_archetype.h"
#include "hs_config.h"
#include "hs_corpus.h"
#include "hs_identity.h"
#include "hs_identity_store.h"
#include "hs_memory.h"
#include "hs_memory_store.h"
#include "hs_queue.h"
#include "hs_style.h"
#include "hs_tier.h"

#include "DBCStores.h"
#include "Group.h"
#include "GroupReference.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Random.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    // Opener trigger tuning -- which shared-context events fire openers,
    // and at what rate before they read as spam -- is a live-realm
    // judgement, not something to settle in advance, so these are compiled
    // constants rather than config keys. 10 minutes and a coin-flip-ish
    // chance are starting guesses, not measurements.
    constexpr uint32_t kOpenerCooldownSeconds = 600;
    constexpr uint32_t kOpenerFireChancePercent = 40;

    // Fifth trigger: how long a (bot, player) pair must be continuously
    // observed in range before "prolonged proximity" counts as a shared
    // moment worth a line. Same starting-guess reasoning as the constants
    // above.
    constexpr uint32_t kProximityDurationThresholdSeconds = 90;

    bool IsBot(Player* p)
    {
        if (!p)
            return false;
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(p);
        return ai && ai->IsBotAI();
    }

    // ---- per (bot, player) opener cooldown -- prevents e.g. a string of
    // joint kills from firing an opener every single time. ----
    std::mutex g_OpenerCooldownMutex;
    std::map<std::pair<uint64_t, uint64_t>, Clock::time_point> g_LastOpenerAt;

    std::atomic<uint32_t> g_OpenersFiredThisSession{0};

    bool OpenerCooldownOk(uint64_t botGuid, uint64_t playerGuid)
    {
        std::lock_guard<std::mutex> lock(g_OpenerCooldownMutex);
        auto it = g_LastOpenerAt.find({ botGuid, playerGuid });
        if (it == g_LastOpenerAt.end())
            return true;
        auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - it->second).count();
        return elapsedSec >= static_cast<int64_t>(kOpenerCooldownSeconds);
    }

    void MarkOpenerFired(uint64_t botGuid, uint64_t playerGuid)
    {
        std::lock_guard<std::mutex> lock(g_OpenerCooldownMutex);
        g_LastOpenerAt[{ botGuid, playerGuid }] = Clock::now();
    }

    // ---- fifth trigger: how long each (bot, player) pair has been
    // continuously observed in range this streak. ----
    std::mutex g_ProximityMutex;
    std::map<std::pair<uint64_t, uint64_t>, Clock::time_point> g_ProximityStartedAt;

    // The one place all five triggers converge: ceiling check, cooldown,
    // chance roll, corpus pick, style pass, delivery. Same "answer without
    // the GPU" shape as hs_handler.cpp's TryReflex/TryGrounded/
    // TryCorpusFallback -- no bucket, no worker thread, and deliberately no
    // history or identity write; openers must never feed interaction score
    // or identity state.
    void FireOpener(Player* bot, Player* player, const char* categoryName)
    {
        if (!g_HsEnable || !bot || !player || !bot->IsInWorld() || !player->IsInWorld())
            return;

        HsTier ceiling = HsParseTier(g_HsMaxTierOpeners);
        if (!HsTierAllows(ceiling, HsTier::Corpus)) // MaxTier.Openers is corpus-only in v1
            return;

        uint64_t botGuid    = bot->GetGUID().GetRawValue();
        uint64_t playerGuid = player->GetGUID().GetRawValue();

        if (!OpenerCooldownOk(botGuid, playerGuid))
            return;
        if (urand(0, 99) >= kOpenerFireChancePercent)
            return;

        std::string line = Hs_SelectOpenerLine(categoryName, bot->getClass(), bot->GetLevel(),
                                                static_cast<uint8_t>(bot->GetTeamId()), bot->GetZoneId());
        if (line.empty())
            return;

        // Same universal-placeholder pass as hs_handler.cpp's
        // TryCorpusFallback -- an opener category is ordinary corpus
        // content, so a row may carry %zone/%class/%level too. No card pass
        // here, since is_opener categories are never card_gated
        // (Hs_SelectOpenerLine enforces that); a card-only token surviving
        // into an opener means a corrupt row, and the leftover check turns
        // that into silence.
        if (line.find('%') != std::string::npos)
        {
            if (!Hs_ResolveUniversalPlaceholders(line, Hs_BuildPlaceholderContext(bot)))
                return;
        }

        HsArchetype             archetype     = Hs_ArchetypeForBot(botGuid, bot->GetLevel());
        const HsArchetypeInfo&  archetypeInfo = Hs_ArchetypeInfoFor(archetype);
        HsStyleContext styleCtx;
        styleCtx.baselineCare         = archetypeInfo.care;
        styleCtx.abbrevOverrideChance = archetypeInfo.hasAbbrevOverride ? archetypeInfo.abbrevOverrideChance : -1.0f;
        styleCtx.inCombat             = bot->IsInCombat();
        styleCtx.verbalTic            = Hs_LookupCardSnapshot(botGuid).verbalTic;
        HsStyleResult style = Hs_ApplyStyle(botGuid, bot->GetName(), player->GetName(), line, styleCtx);
        if (style.text.empty())
            return;

        Hs_DeliverReflexReply(botGuid, playerGuid, HsReplyChannel::Say, style.text);
        MarkOpenerFired(botGuid, playerGuid);
        g_OpenersFiredThisSession.fetch_add(1);
    }
}

void HsOpenerGroupHandler::OnAddMember(Group* group, ObjectGuid guid)
{
    if (!group)
        return;
    Player* newMember = ObjectAccessor::FindPlayer(guid);
    if (!newMember || !newMember->IsInWorld())
        return;

    bool newIsBot = IsBot(newMember);

    // Find "the other side": if the joiner is a bot, the bot it should
    // greet is itself and the target is the first real player already in
    // the group; if the joiner is a real player, the greeter is the first
    // bot already there. Either way this fires once per OnAddMember call,
    // not once per bot in the group.
    Player* bot    = newIsBot ? newMember : nullptr;
    Player* player = newIsBot ? nullptr   : newMember;

    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || member == newMember || !member->IsInWorld())
            continue;

        if (newIsBot && !player && !IsBot(member))
            player = member;
        else if (!newIsBot && !bot && IsBot(member))
            bot = member;

        if (bot && player)
            break;
    }

    if (bot && player)
    {
        // "Grouped in a zone" is a shared-experience memory beat
        // independent of whether an opener actually fires -- not a player
        // utterance, so it's recorded here at the trigger site rather than
        // in hs_queue.cpp's WorkerLoop, same as the dungeon-completion
        // score bump below in HsOpenerEncounterHandler.
        AreaTableEntry const* entry = sAreaTableStore.LookupEntry(bot->GetZoneId());
        const char* zoneName = entry ? entry->area_name[0] : nullptr;
        std::string zone = (zoneName && *zoneName) ? zoneName : "the field";
        Hs_RecordMemoryEvent(bot->GetGUID().GetRawValue(), player->GetGUID().GetRawValue(),
                              kHsMemoryEventGroupedInZone, Hs_BuildGroupedInZoneText(zone));

        FireOpener(bot, player, "opener_group_formed");
    }
}

void HsOpenerKillHandler::OnPlayerCreatureKill(Player* killer, Creature* /*killed*/)
{
    // Only fires when a bot lands the killing blow -- OnPlayerCreatureKill
    // fires once per player who does, not once per player with kill
    // credit, so "jointly" is scoped to this direction rather than a
    // cross-player correlation cache (hs_opener.h).
    if (!killer || !IsBot(killer))
        return;

    Group* group = killer->GetGroup();
    if (!group)
        return;

    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || member == killer || !member->IsInWorld() || IsBot(member))
            continue;
        FireOpener(killer, member, "opener_joint_kill");
        break; // one opener per kill, not one per real player in the group
    }
}

void HsOpenerResurrectHandler::OnPlayerResurrect(Player* player, float /*restorePercent*/, bool& /*applySickness*/)
{
    // Scoped to the bot-receives-rez direction -- the hook carries no
    // caster/giver reference (hs_opener.h).
    if (!player || !IsBot(player))
        return;

    Group* group = player->GetGroup();
    if (!group)
        return;

    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || member == player || !member->IsInWorld() || IsBot(member))
            continue;
        FireOpener(player, member, "opener_rez");
        break;
    }
}

void HsOpenerEncounterHandler::OnAfterUpdateEncounterState(Map* map, EncounterCreditType /*type*/, uint32_t /*creditEntry*/,
                                                             Unit* /*source*/, Difficulty /*difficultyFixed*/,
                                                             DungeonEncounterList const* /*encounters*/,
                                                             uint32_t dungeonCompleted, bool /*updated*/)
{
    if (!dungeonCompleted || !map) // nonzero only on the dungeon's actual last encounter, not every boss
        return;

    Player* bot    = nullptr;
    Player* player = nullptr;
    Map::PlayerList const& players = map->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player* member = itr->GetSource();
        if (!member || !member->IsInWorld())
            continue;
        if (IsBot(member))
        {
            if (!bot)
                bot = member;
        }
        else if (!player)
        {
            player = member;
        }
        if (bot && player)
            break;
    }

    if (bot && player)
    {
        // "Dungeon completed together" is a shared-experience signal
        // independent of whether an opener fires -- not a player
        // utterance, so it's scored here rather than in hs_queue.cpp's
        // WorkerLoop. One representative bot/player pair; exact
        // multiplicity across a multi-bot group doesn't need to be exact.
        Hs_BumpInteractionScore(bot->GetGUID().GetRawValue(), bot->GetLevel(), kHsScoreWeightDungeonComplete);

        // Uses Map::GetMapName() rather than resolving dungeonCompleted's
        // LFG dungeon id to a display name; the map's own name is real,
        // always available, and equally truthful for this purpose.
        Hs_RecordMemoryEvent(bot->GetGUID().GetRawValue(), player->GetGUID().GetRawValue(),
                              kHsMemoryEventDungeonCompleted, Hs_BuildDungeonCompletedText(map->GetMapName()));

        FireOpener(bot, player, "opener_dungeon_complete");
    }
}

void Hs_ScanProximityOpeners()
{
    std::vector<Player*> realPlayers;
    std::vector<Player*> bots;
    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        Player* candidate = itr.second;
        if (!candidate || !candidate->IsInWorld())
            continue;
        if (IsBot(candidate))
            bots.push_back(candidate);
        else
            realPlayers.push_back(candidate);
    }

    std::set<std::pair<uint64_t, uint64_t>> currentlyObserved;
    for (Player* player : realPlayers)
    {
        for (Player* bot : bots)
        {
            if (bot->GetTeamId() != player->GetTeamId())
                continue;
            if (!bot->IsAlive() || bot->IsInCombat())
                continue;
            if (bot->GetDistance(player) > g_HsSayDistance)
                continue;
            currentlyObserved.insert({ bot->GetGUID().GetRawValue(), player->GetGUID().GetRawValue() });
        }
    }

    std::vector<std::pair<uint64_t, uint64_t>> toFire;
    {
        std::lock_guard<std::mutex> lock(g_ProximityMutex);
        Clock::time_point now = Clock::now();

        // A pair that dropped out of range (or where either side left/died/
        // entered combat) loses its streak entirely -- "prolonged" means
        // continuous, not cumulative.
        for (auto it = g_ProximityStartedAt.begin(); it != g_ProximityStartedAt.end(); )
        {
            if (!currentlyObserved.count(it->first))
                it = g_ProximityStartedAt.erase(it);
            else
                ++it;
        }

        for (auto const& pairKey : currentlyObserved)
        {
            auto it = g_ProximityStartedAt.find(pairKey);
            if (it == g_ProximityStartedAt.end())
            {
                g_ProximityStartedAt[pairKey] = now;
                continue;
            }
            auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
            if (elapsedSec >= static_cast<int64_t>(kProximityDurationThresholdSeconds))
            {
                toFire.push_back(pairKey);
                it->second = now; // restart the streak -- FireOpener's own per-pair cooldown gates repeats from here
            }
        }
    }

    for (auto const& pairKey : toFire)
    {
        Player* bot    = ObjectAccessor::FindPlayer(ObjectGuid(pairKey.first));
        Player* player = ObjectAccessor::FindPlayer(ObjectGuid(pairKey.second));
        if (bot && player)
            FireOpener(bot, player, "opener_prolonged_proximity");
    }
}

uint32_t Hs_OpenersFiredThisSession()
{
    return g_OpenersFiredThisSession.load();
}
