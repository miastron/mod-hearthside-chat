#include "hs_opener.h"
#include "hs_ambient.h"
#include "hs_archetype.h"
#include "hs_config.h"
#include "hs_corpus.h"
#include "hs_identity.h"
#include "hs_identity_store.h"
#include "hs_memory.h"
#include "hs_memory_store.h"
#include "hs_prune.h"
#include "hs_queue.h"
#include "hs_rpgstate.h"
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
    // judgement, not something to settle in advance, so this is a compiled
    // constant rather than a config key. 10 minutes is a starting guess,
    // not a measurement.
    //
    // The fire chance used to sit here on the same footing. It moved to
    // Openers.FireChancePercent (hs_config.h) when the settled-state gate
    // landed: that gate's size depends on live realm conditions, so the
    // number that compensates for it has to be adjustable without a
    // rebuild.
    constexpr uint32_t kOpenerCooldownSeconds = 600;

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

    // HearthsideChat.ExcludeNames keeps a named bot out of this module
    // entirely -- "no reflex, grounded, corpus, or reactive reply, ever"
    // (hs_config.h). An opener is a corpus line the bot speaks unprompted, so
    // the rule has to hold here too; every chat hook in hs_handler.cpp,
    // hs_event.cpp and hs_botchain.cpp already applies it at its own
    // candidate scan.
    //
    // Deliberately a second predicate rather than folded into IsBot() above:
    // an excluded bot is still a bot, and the trigger handlers below use
    // IsBot() to tell bots from real players. Collapsing the two would make
    // an excluded bot register as the *human* side of a pair and fire an
    // opener at it from some other bot.
    bool IsEligibleBot(Player* p)
    {
        return IsBot(p) && !Hs_IsExcludedBotName(p->GetName());
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
        Clock::time_point now = Clock::now();
        g_LastOpenerAt[{ botGuid, playerGuid }] = now;

        // Keyed by (bot, player), so this grows with the *product* of the two
        // populations rather than either one -- worth pruning even though the
        // value is only a timestamp. OpenerCooldownOk reads it purely through
        // kOpenerCooldownSeconds, so an entry four windows old already
        // answers exactly as a missing one does (hs_prune.h).
        HsPrune::PruneStale(g_LastOpenerAt, now,
                            /*staleSeconds=*/static_cast<int64_t>(kOpenerCooldownSeconds) * 4,
                            /*pruneAboveSize=*/512);
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
    //
    // `channel` is Say for every trigger whose shared moment is a physical
    // one -- a kill, a rez, a dungeon's last boss, two strangers standing in
    // the same field -- where the audience is whoever is close enough to see
    // what just happened. opener_group_formed is the exception: the moment
    // being remarked on is joining the group itself, its audience is the
    // group, and the joining bot may be nowhere near the player who invited
    // it (mod-playerbots only teleports it in when summonWhenGroup is set and
    // it is out of sight distance), so a /say there is frequently addressed
    // to an empty patch of ground.
    void FireOpener(Player* bot, Player* player, const char* categoryName,
                     HsReplyChannel channel = HsReplyChannel::Say)
    {
        if (!g_HsEnable || !bot || !player || !bot->IsInWorld() || !player->IsInWorld())
            return;

        // Backstop for HearthsideChat.ExcludeNames. Each trigger below
        // already filters with IsEligibleBot at its own selection site --
        // it has to, because two of them write an hside_memory row before
        // calling in here -- but this is the one funnel all five converge
        // on, so it is also the one place a future trigger cannot forget.
        if (Hs_IsExcludedBotName(bot->GetName()))
            return;

        HsTier ceiling = HsParseTier(g_HsMaxTierOpeners);
        if (!HsTierAllows(ceiling, HsTier::Corpus)) // MaxTier.Openers is corpus-only in v1
            return;

        uint64_t botGuid    = bot->GetGUID().GetRawValue();
        uint64_t playerGuid = player->GetGUID().GetRawValue();

        if (!OpenerCooldownOk(botGuid, playerGuid))
            return;

        // The settled-state gate (Hs_IsBotSettled, hs_rpgstate.h), on the
        // /say path only. A bot that stops mid-run to greet someone it is
        // jogging past is the same tell ambient chatter had; a bot parked at
        // an inn or camping a spawn greeting a passer-by is not.
        //
        // opener_group_formed is the exception, and it is why this tests the
        // channel rather than applying unconditionally: its moment is the
        // grouping itself, it is delivered to party/raid, and a bot that was
        // travelling when the player invited it should still acknowledge the
        // invite. Suppressing that would be the reverse of the problem this
        // gate solves -- silence where a response is expected.
        if (channel == HsReplyChannel::Say && !Hs_IsBotSettled(bot))
            return;

        if (urand(0, 99) >= g_HsOpenerFireChancePercent)
            return;

        // PLAN-AMBIENT.md §2: the shared unprompted-speech budget
        // (hs_queue.h). An opener is one of three producers that speaks
        // without anyone having said anything, so it now spends from the
        // same budget ambient chatter and scripted scenes do rather than
        // relying only on its own per-pair cooldown -- that cooldown bounds
        // how often *this pair* hears an opener, not how much unprompted
        // speech the realm produces in total.
        //
        // Deliberately after the cooldown and chance roll, both of which are
        // in-memory, and before Hs_SelectOpenerLine's DB query: a token
        // should be spent on a line that is actually about to be attempted,
        // not burned by a bot that was going to stay quiet anyway.
        if (!Hs_AmbientBucketTake())
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
        HsArchetypeInfo const   archetypeInfo = Hs_ArchetypeInfoFor(archetype);
        HsStyleContext styleCtx;
        styleCtx.baselineCare         = archetypeInfo.care;
        styleCtx.abbrevOverrideChance = archetypeInfo.hasAbbrevOverride ? archetypeInfo.abbrevOverrideChance : -1.0f;
        styleCtx.inCombat             = bot->IsInCombat();
        styleCtx.verbalTic            = Hs_LookupCardSnapshot(botGuid).verbalTic;
        // §4.17: set here for the same reason inCombat is. The sighting is
        // per-bot and time-decayed, not per-surface -- a bot that just
        // watched a WTS flurry in Trade is keyed up whatever it says next,
        // and an opener is the one HsStyleContext site that used to miss it.
        styleCtx.tradeCareOffset      = Hs_TradeCareOffsetFor(botGuid);
        HsStyleResult style = Hs_ApplyStyle(botGuid, bot->GetName(), player->GetName(), line, styleCtx);
        if (style.text.empty())
            return;

        Hs_DeliverReflexReply(botGuid, playerGuid, channel, style.text);
        MarkOpenerFired(botGuid, playerGuid);

        // The per-pair opener cooldown above says nothing about ambient, which
        // is keyed per bot and has no idea an opener just went out. Without
        // this a bot could greet the group and then, seconds later on the next
        // ambient tick, follow it with an unrelated ambient_party_downtime
        // line -- two unprompted lines from the same bot back to back, which
        // is exactly what a player sees as bot spam (hs_ambient.h).
        Hs_MarkAmbientSpoke(botGuid);

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

    // An excluded bot joining is not a greeter and is not the human side
    // either, so there is nothing to do for this call at all -- and bailing
    // here (rather than inside FireOpener) is what keeps the grouped_in_zone
    // memory write below from happening for it.
    if (newIsBot && !IsEligibleBot(newMember))
        return;

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

        // IsBot for the human-side test, IsEligibleBot for the greeter:
        // an excluded bot is neither, so it is skipped as a greeter without
        // ever being mistaken for the real player.
        if (newIsBot && !player && !IsBot(member))
            player = member;
        else if (!newIsBot && !bot && IsEligibleBot(member))
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

        // Party/raid, not /say: see FireOpener's `channel` note. Decided off
        // the `group` this hook was handed rather than bot->GetGroup(), which
        // is not guaranteed to be wired up yet at OnAddMember time; by the
        // time Hs_DeliverPending calls SayToParty/SayToRaid (a 400-1500ms
        // reflex delay later) it is, and SayToRaid is the one that needs the
        // raid flag to be right.
        FireOpener(bot, player, "opener_group_formed",
                    group->isRaidGroup() ? HsReplyChannel::Raid : HsReplyChannel::Party);
    }
}

void HsOpenerKillHandler::OnPlayerCreatureKill(Player* killer, Creature* /*killed*/)
{
    // Only fires when a bot lands the killing blow -- OnPlayerCreatureKill
    // fires once per player who does, not once per player with kill
    // credit, so "jointly" is scoped to this direction rather than a
    // cross-player correlation cache (hs_opener.h).
    if (!killer || !IsEligibleBot(killer)) // ExcludeNames: never the speaker
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
    if (!player || !IsEligibleBot(player)) // ExcludeNames: never the speaker
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
        // IsBot decides which side of the split this member is on;
        // IsEligibleBot decides whether it may be the speaker. An excluded
        // bot therefore lands in neither slot -- it must not be picked as
        // the greeter, and must not fall through to the `player` branch and
        // be treated as the human the dungeon was run with (which would also
        // write it an hside_memory row below).
        if (IsBot(member))
        {
            if (!bot && IsEligibleBot(member))
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
        // Three-way, not two: an ExcludeNames bot belongs in neither list.
        // Dropping it from `bots` is the actual fix; keeping it out of
        // `realPlayers` is what stops the plain `else` from promoting it to
        // a human that other bots then open on.
        if (IsBot(candidate))
        {
            if (IsEligibleBot(candidate))
                bots.push_back(candidate);
        }
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
            // A grouped bot is already at this player's side by the
            // player's own choice -- "we've been standing around a while"
            // is not a shared moment worth remarking on when that's just
            // what a party follower does. opener_group_formed already
            // covers the moment grouping itself happens; this trigger is
            // for two strangers who happen to keep crossing paths.
            if (bot->GetGroup() && bot->GetGroup() == player->GetGroup())
                continue;
            // Map- and phase-aware; see hs_handler.cpp's /say eligibility
            // filter for why the bare GetDistance is wrong here.
            if (!bot->IsWithinDistInMap(player, g_HsSayDistance))
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
