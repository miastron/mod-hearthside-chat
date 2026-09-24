#include "hs_event.h"
#include "hs_archetype.h"
#include "hs_bot.h"
#include "hs_config.h"
#include "hs_event_arbiter.h"
#include "hs_log.h"
#include "hs_queue.h"
#include "hs_tier.h"
#include "hs_locale.h"
#include "hs_prune.h"

#include "Battleground.h"
#include "Creature.h"
#include "Group.h"
#include "GroupReference.h"
#include "GroupMgr.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SharedDefines.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    std::atomic<uint32_t> g_EventsFiredThisSession{ 0 };

    // ---- Why no trigger below names a zone ---------------------------------
    // Claude/archive/PLAN-ARBITER.md §6 originally called for a bracketed "[Elwynn Forest] ..."
    // state line on every non-death trigger. Dropped 2026-08-24 (operator
    // decision) for three reasons, in order of weight:
    //
    // 1. It is already in the prompt. Hs_MakeReplyRequest (hs_queue.h) reads the
    //    zone (and the instance name inside a dungeon), and hs_topic_gate.cpp
    //    turns it into "You are currently in Elwynn Forest." on the persona
    //    line for every one of these requests. A bracketed copy states it
    //    twice.
    // 2. It is the one detail no reaction is ever *about*. Nobody reacts to a
    //    ding because of the zone, so the token is surface area for the model
    //    to invent around for no gain.
    // 3. No bracket in the user slot means no bracket syntax for the model to
    //    echo back into a reply, the risk lint_dataset.py's ROLEPLAY guard
    //    was widened to catch. That guard stays as belt-and-braces.
    //
    // Reason 2 used to carry more weight than it does now, and the difference
    // is worth being explicit about, because it is what unblocked the killer
    // name below. It read: a zone name is the highest-risk kind of token,
    // since a model with any WoW pretraining can volunteer Goldshire or
    // Hogger off "Elwynn Forest", none of it stated and some of it wrong.
    // That was §7 rule 1's reasoning ("write to the ignorance"), and it was a
    // workaround for having no way to make a stated detail *true*. hs_rag.h
    // is that way: a named noun now arrives at the backend with an authored
    // paragraph retrieved beside it (hs_queue.cpp). So the rule is no longer
    // "state nothing the model could invent around" but "state nothing the
    // corpus cannot back" -- and the zone stays out on reason 1 alone, which
    // never depended on the model's priors.
    //
    // Player and item names were always exempt from this and stay in the
    // triggers below: the reaction *is* the name ("stay down, grimtusk",
    // "gz faeltha"), stripping the addressee would force the model to guess
    // one, and unlike a zone they are runtime-substituted proper nouns it has
    // no priors about: all it can do is repeat them.

    // ---- Deferred death dispatch -------------------------------------------
    // A death event cannot name its killer at the moment it fires, because
    // there is no killer yet. Unit::Kill calls victim->setDeathState(JustDied)
    // near its top, which is what runs Player::setDeathState and from there
    // OnPlayerJustDied; it only reaches OnPlayerKilledByCreature much further
    // down the same function (Unit.cpp:14311, read against
    // azerothcore-wotlk-pb 2026-09-07). The two hooks arrive in exactly the
    // wrong order for "killed by Prince Taldaram".
    //
    // So OnPlayerJustDied records the death instead of dispatching it,
    // OnPlayerKilledByCreature fills in the killer microseconds later in that
    // same Unit::Kill, and DrainPendingDeaths dispatches from
    // HsEventDeathDrainWorldScript::OnUpdate. World::Update runs
    // sMapMgr->Update *before* sScriptMgr->OnWorldUpdate (World.cpp:1245 and
    // 1346), so a death during the map update is normally drained later in
    // that same tick, not the next one -- but the drain re-resolves every
    // Player* through ObjectAccessor regardless rather than holding a pointer
    // across the boundary, since nothing in the hook contract promises that
    // ordering.
    //
    // Locked, and the reason is worth stating because the obvious assumption
    // is wrong: PlayerScript hooks are NOT world-thread only. Unit::Kill runs
    // inside a map update, and MapMgr schedules map updates onto a pool of
    // MapUpdate.Threads workers (MapMgr.cpp:271) -- the test realm runs 8. So
    // two deaths on two different maps reach the hooks below genuinely
    // concurrently, and an unguarded push_back here would be a data race, not
    // a theoretical one. hs_queue.cpp's review-B7 note records the same
    // discovery arriving as an observed bug: concurrent callers both passed a
    // token-bucket peek and drove it negative.
    //
    // The drain does not need the lock for its own sake -- MapMgr::Update
    // calls m_updater.wait() before returning, so every map thread has joined
    // by the time World::Update reaches OnWorldUpdate -- but it takes it
    // anyway rather than resting the safety of the whole mechanism on that
    // one ordering fact holding in some future core version.
    struct HsPendingDeath
    {
        ObjectGuid  playerGuid;
        std::string killerName; // "" for a fall, a drowning, or a PvP kill
    };

    std::mutex                  g_PendingDeathMutex;
    std::vector<HsPendingDeath> g_PendingDeaths;

    // A raid wiping queues one record per corpse, which is the intended
    // shape (the drain collapses them into a single wipe event). The cap is
    // only here so that a bug in the drain cannot turn this into unbounded
    // growth; 64 is comfortably above a full raid.
    constexpr size_t kMaxPendingDeaths = 64;

    // One actor the fire site is offering to the arbiter: the bot, how
    // involved it is, which event type its *own* affinity resolves against,
    // and the trigger text to send if it is the one selected. Keeping the
    // trigger per-actor is what lets a duel end arbitrate once over
    // {winner, loser} and still say the right thing to whichever side wins
    // the draw (Claude/archive/PLAN-ARBITER.md §2).
    //
    // `counterpart` is the other person in it, when there is one: the duel
    // opponent, the groupmate who died, the player who rezzed this bot. It
    // becomes the reaction's sender (so a duel-start line is no longer logged
    // as the bot talking to itself) and it is who the follow-up context
    // recognises as "the one talking to you now" (Hs_RecentEventContext).
    struct HsEventActor
    {
        Player*            bot;
        HsEventInvolvement involvement;
        HsEventType        affinityType;
        std::string        trigger;
        Player*            counterpart = nullptr;
    };

    // ---- What just happened, for the replies that come after it -----------
    //
    // Until 2026-09-23 an event reached exactly one prompt: the reaction's,
    // if the arbiter picked this bot to make one. A player who then talked to
    // the bot about it ("loser", "gz", "ty for the rez") was answered by a bot
    // with no idea anything had happened -- the duel outcome's only other
    // route into a reply was the experience block, framed as background not
    // to be brought up, and measured doing nothing for exactly this case
    // (Tests/post_event_probe.py). So every actor of every event gets the
    // same state line its reaction would have carried, kept for a short
    // window and appended to its replies by hs_queue.cpp's WorkerLoop, in the
    // slot the event reaction itself uses.
    //
    // Recorded for every eligible actor, before the ceiling, budget and
    // audience gates and regardless of who the arbiter picks: a duel end is
    // silent three times in four by design, and that silence is exactly when
    // the follow-up matters most. One entry per bot, newest wins -- a duel's
    // end replaces its start.
    struct HsRecentEvent
    {
        std::string                           line;
        uint64_t                              counterpartGuid = 0;
        std::string                           counterpartName;
        std::chrono::steady_clock::time_point at;
    };

    // Two minutes: long enough to cover "gg" typed after a duel or a "wb"
    // after a login, short enough that "has just" is still true. A starting
    // judgement, same status as the bias table.
    constexpr int64_t kRecentEventWindowSeconds  = 120;
    constexpr size_t  kRecentEventPruneThreshold = 512;

    std::mutex                                  g_RecentEventMutex;
    std::unordered_map<uint64_t, HsRecentEvent> g_RecentEvents;

    void NoteRecentEvent(Player* bot, std::string const& line, Player* counterpart)
    {
        HsRecentEvent entry;
        entry.line = line;
        if (counterpart && counterpart != bot)
        {
            entry.counterpartGuid = counterpart->GetGUID().GetRawValue();
            entry.counterpartName = counterpart->GetName();
        }
        auto const now = std::chrono::steady_clock::now();
        entry.at = now;

        std::lock_guard<std::mutex> lock(g_RecentEventMutex);
        g_RecentEvents[bot->GetGUID().GetRawValue()] = std::move(entry);
        HsPrune::PruneStale(g_RecentEvents, now, kRecentEventWindowSeconds * 4, kRecentEventPruneThreshold,
                            [](HsRecentEvent const& e) { return e.at; });
    }

    bool EligibleBot(Player* bot)
    {
        if (!bot || !Hs_IsBot(bot) || !bot->IsInWorld())
            return false;
        // HearthsideChat.ExcludeNames: never spoken through, no tier at
        // all, same rule every chat surface applies.
        if (Hs_IsExcludedBotName(bot->GetName()))
            return false;
        // Deliberately no g_HsDisableRepliesInCombat check here: see the
        // combat-gate note in hs_event.h.
        return true;
    }

    // Defined below, next to NearbyBots, since it is the same kind of scan;
    // declared here because FireEvent is the only caller and reads better
    // before the helpers it uses.
    bool RealPlayerInSayRange(Player* origin);

    // The one place every trigger converges: ceiling, event budget, audience,
    // candidate assembly, arbitration, dispatch.
    //
    // `origin` is whoever the event happened around: the player who died,
    // the one who dinged, the duel opponent. It becomes the request's
    // "sender", which is what the style pass protects from typo injection
    // and what proximity is measured from. Nothing is scored against it:
    // Hs_TryEnqueue's isEvent flag suppresses the history append, the
    // interaction-score bump, and the first-meeting record, so an origin
    // that is itself a bot can never seed identity state.
    void FireEvent(HsEventType primaryType, Player* origin, std::vector<HsEventActor>& actors, HsReplyChannel channel)
    {
        if (!g_HsEnable || !origin || !origin->IsInWorld() || actors.empty())
            return;

        // The follow-up fact first, for everyone it happened to, whatever the
        // gates below decide about a reaction (see HsRecentEvent above).
        for (auto const& actor : actors)
            if (EligibleBot(actor.bot))
                NoteRecentEvent(actor.bot, actor.trigger, actor.counterpart);

        // A ceiling is permission, not budget. Events have no corpus
        // fallback: a canned line reacting to a specific death or roll
        // would have to be generic enough to be wrong most of the time, so
        // anything below inference is silence, not a downgrade.
        HsTier ceiling = g_HsMaxTierEvents;
        if (!HsTierAllows(ceiling, HsTier::Inference))
            return;

        // Spent once per event, before any per-candidate work, so a busy
        // dungeon's stream of deaths and rolls costs almost nothing when the
        // budget is already gone. This bucket is deliberately separate from
        // the tier-2 reply bucket: unbudgeted ambient reactions would
        // otherwise starve replies to players who actually spoke, which is
        // the thing players notice (Claude/archive/PLAN-ARBITER.md §8).
        if (!Hs_EventBucketTake())
            return;

        // Audience gate, /say only: party and raid carry their own. Refunds
        // the token it just spent -- nothing was ever going to be heard, so
        // this must not cost the budget the way a real reaction does (the
        // same reasoning as the empty-candidates refund below).
        if (channel == HsReplyChannel::Say && !RealPlayerInSayRange(origin))
        {
            Hs_EventBucketRefund();
            return;
        }

        std::vector<HsEventCandidate> candidates;
        std::vector<Player*>          bots;
        std::vector<Player*>          counterparts;
        candidates.reserve(actors.size());
        bots.reserve(actors.size());
        counterparts.reserve(actors.size());

        for (auto const& actor : actors)
        {
            if (!EligibleBot(actor.bot))
                continue;

            uint64_t botGuid = actor.bot->GetGUID().GetRawValue();

            HsEventCandidate candidate;
            candidate.botGuid               = botGuid;
            candidate.sameMap               = (actor.bot->GetMapId() == origin->GetMapId());
            candidate.distance              = candidate.sameMap ? actor.bot->GetDistance(origin) : 0.0f;
            candidate.secondsSinceLastReply = Hs_SecondsSinceLastReply(botGuid);
            candidate.involvement           = actor.involvement;
            candidate.trigger               = actor.trigger;

            HsArchetype archetype = Hs_ArchetypeForBot(botGuid);
            candidate.affinityWeight = Hs_EventAffinityWeight(actor.affinityType,
                Hs_ArchetypeInfoFor(archetype).enumName);

            candidates.push_back(std::move(candidate));
            bots.push_back(actor.bot);
            counterparts.push_back(actor.counterpart);
        }

        if (candidates.empty())
        {
            // Review C2: the event token was spent before EligibleBot
            // filtering, so an event whose actors are all excluded (bots
            // offline, in an excluded-name list, already mid-script) used to
            // cost a token for a reaction nobody could ever have heard.
            Hs_EventBucketRefund();
            return;
        }

        std::vector<size_t> selected = Hs_ArbitrateEventReplies(
            Hs_EventCountBiasFor(primaryType), g_HsSayDistance, candidates);

        for (size_t index : selected)
        {
            Player* bot = bots[index];

            // The other person in the event where there is one, so the
            // request (and hside_chat_log) names who the line is aimed at
            // rather than the origin, which for a duel or a solo death is the
            // bot itself.
            Player* sender = (counterparts[index] && counterparts[index]->IsInWorld()) ? counterparts[index] : origin;

            HsReplyRequest request = Hs_MakeReplyRequest(bot, sender, channel, candidates[index].trigger);
            request.isEvent            = true;
            request.triggerIsStateLine = true;

            if (Hs_TryEnqueue(std::move(request)))
                g_EventsFiredThisSession.fetch_add(1);
            else if (g_HsDebugEnabled)
                LOG_INFO(kHsLogChat, "[HearthsideChat] Event {} enqueue rejected for bot {}.",
                    Hs_EventTypeName(primaryType), bot->GetName());
        }
    }

    // Party or raid, chosen by the group's own shape: PlayerbotAI::SayToParty
    // and ::SayToRaid are different calls, so this is a delivery fact, not a
    // cosmetic one (hs_queue.h).
    HsReplyChannel GroupChannelFor(Group* group)
    {
        return (group && group->isRaidGroup()) ? HsReplyChannel::Raid : HsReplyChannel::Party;
    }

    // Bots within say range of the origin, on the same map and phase, same
    // faction. Used only by world-scoped events, which deliberately do not
    // require a real player nearby.
    std::vector<Player*> NearbyBots(Player* origin, Player* exclude)
    {
        std::vector<Player*> found;
        for (auto const& itr : ObjectAccessor::GetPlayers())
        {
            Player* candidate = itr.second;
            if (!candidate || candidate == origin || candidate == exclude)
                continue;
            if (!EligibleBot(candidate))
                continue;
            if (candidate->GetTeamId() != origin->GetTeamId())
                continue; // opposing faction can't read /say
            // IsWithinDistInMap, not GetDistance: GetPlayers() is realm-wide
            // and a bare distance is map- and phase-blind, so two instances
            // of one dungeon would read as the same room (hs_handler.cpp).
            if (!candidate->IsWithinDistInMap(origin, g_HsSayDistance))
                continue;
            found.push_back(candidate);
        }
        return found;
    }

    // Is there a real player close enough to read a /say line from here?
    //
    // Events had no audience test of any kind before 2026-09-13, and /say is
    // the one channel where that matters: a party or raid reaction has the
    // group as its audience by construction, but a solo death fires wherever
    // the bot happened to die. On the test realm that was 103 of 333 logged
    // reactions -- roughly a third of everything the module said all day --
    // and almost none of it had a human within a hundred yards. Each one
    // still cost an LLM call and a token from the event bucket, so the
    // budget meant to cover reactions players actually witness was being
    // spent on an empty hillside.
    //
    // Unconditional rather than a config key: a line nobody can hear has no
    // value at any setting, which is not true of the ambient surfaces that
    // do give the operator an Ambient.RequireRealPlayer switch.
    //
    // Cost is bounded by the event token bucket, not by how often things die
    // -- the caller spends a token first, so this walk runs at most
    // Events.Bucket.RepliesPerMinute times a minute.
    bool RealPlayerInSayRange(Player* origin)
    {
        for (auto const& itr : ObjectAccessor::GetPlayers())
        {
            Player* candidate = itr.second;
            // The origin counts when it is a real player: until 2026-09-23 it
            // was skipped, which was harmless while origins were bots (a solo
            // death, a killing blow) and silenced every event whose origin is
            // the person the bot answers (a rez, a trade, a duel player1).
            if (!candidate || !candidate->IsInWorld())
                continue;
            // An excluded bot is not a real player: the same three-way
            // distinction hs_ambient.cpp's scan documents. Hs_IsBot alone
            // would be enough here, but reading it this way keeps the two
            // scans saying the same thing.
            if (Hs_IsBot(candidate))
                continue;
            if (candidate->GetTeamId() != origin->GetTeamId())
                continue; // opposing faction can't read /say
            if (!candidate->IsWithinDistInMap(origin, g_HsSayDistance))
                continue;
            return true;
        }
        return false;
    }
}

namespace
{
    // The death logic itself, one drain later and with the killer filled in
    // where there was one. Structurally the old
    // HsEventDeathHandler::OnPlayerJustDied body; `wipedGroups` is the single
    // addition deferral forced, see the wipe branch.
    void FireDeathEvent(Player* player, const std::string& killerName, std::unordered_set<uint64_t>& wipedGroups)
    {
        // " by Prince Taldaram", or "" when nothing named killed this player
        // -- a fall, a drowning, or an enemy player, none of which reach
        // OnPlayerKilledByCreature. A trigger reading "killed by ." is worse
        // than one that never mentions a killer, so the clause is built once
        // and appended rather than templated in.
        //
        // Creature::GetName() and not a hs_locale.h helper, deliberately:
        // hside_rag is authored in English and keyed on English titles, so a
        // localized name would name the boss correctly and then retrieve
        // nothing for it. The module's "never index a name array directly"
        // rule is about DBC/template arrays; this is the object's own
        // resolved name.
        const std::string by = killerName.empty() ? "" : (" by " + killerName);

        Group* group = player->GetGroup();
        std::vector<HsEventActor> actors;

        if (group)
        {
            // A wipe is checked first because it supersedes the individual
            // death: if this death left nobody standing, the group has one
            // thing to react to, not one per corpse.
            bool anyoneAlive = false;
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (member && member->IsInWorld() && member != player && member->IsAlive())
                {
                    anyoneAlive = true;
                    break;
                }
            }

            if (!anyoneAlive)
            {
                // The one thing deferral cost. Firing straight out of
                // OnPlayerJustDied used to dedup a wipe for free: of two members
                // dying in the same tick, the first still saw the second alive,
                // so only the death that completed the wipe took this branch. A
                // drain sees both corpses and would fire twice, so the group is
                // claimed here instead.
                uint64_t groupId = group->GetGUID().GetRawValue();
                if (!wipedGroups.insert(groupId).second)
                    return;

                // Second person for every member: a wipe happened to all of
                // them, so there is no witnessed-from-outside phrasing to pick.
                for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                {
                    Player* member = itr->GetSource();
                    if (!member || !EligibleBot(member))
                        continue;
                    actors.push_back({ member,
                        member == player ? HsEventInvolvement::Subject : HsEventInvolvement::Affected,
                        HsEventType::DeathWipe,
                        "Your whole group has just been wiped out" + by + "." });
                }
                FireEvent(HsEventType::DeathWipe, player, actors, GroupChannelFor(group));
                return;
            }

            if (Hs_IsBot(player))
            {
                if (!EligibleBot(player))
                    return;
                actors.push_back({ player, HsEventInvolvement::Subject, HsEventType::DeathInGroup,
                    "You are in a group and have just been killed" + by + "." });
                FireEvent(HsEventType::DeathInGroup, player, actors, GroupChannelFor(group));
                return;
            }

            // A real player in the group died. Candidates are the group's bots,
            // who watched it happen: third person, naming only the fact the
            // trigger states.
            std::string trigger = std::string(player->GetName()) + ", in your group, has just been killed" + by + ".";
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (!member || member == player || !EligibleBot(member))
                    continue;
                actors.push_back({ member, HsEventInvolvement::Affected, HsEventType::DeathGroupPlayer, trigger, player });
            }
            FireEvent(HsEventType::DeathGroupPlayer, player, actors, GroupChannelFor(group));
            return;
        }

        // Ungrouped. Only a bot's own solo death is a trigger: a stranger
        // dying nearby is not one of Claude/archive/PLAN-ARBITER.md §5's sixteen.
        if (!Hs_IsBot(player) || !EligibleBot(player))
            return;

        actors.push_back({ player, HsEventInvolvement::Subject, HsEventType::DeathSolo,
            "You were out on your own and have just been killed" + by + "." });
        FireEvent(HsEventType::DeathSolo, player, actors, HsReplyChannel::Say);
    }

    // Dispatches every death recorded since the last call. Re-resolves each
    // Player* rather than trusting a pointer taken at hook time.
    void DrainPendingDeaths()
    {
        // Swapped out under the lock and then processed outside it:
        // FireDeathEvent runs arbitration and reaches Hs_TryEnqueue, which
        // takes locks of its own, and holding this one across that would put
        // an ordering edge between them for no reason.
        std::vector<HsPendingDeath> batch;
        {
            std::lock_guard<std::mutex> lock(g_PendingDeathMutex);
            if (g_PendingDeaths.empty())
                return;
            batch.swap(g_PendingDeaths);
        }

        std::unordered_set<uint64_t> wipedGroups;
        for (HsPendingDeath const& pending : batch)
        {
            Player* player = ObjectAccessor::FindPlayer(pending.playerGuid);
            if (!player || !player->IsInWorld())
                continue; // logged out or despawned between the death and here
            FireDeathEvent(player, pending.killerName, wipedGroups);
        }
    }
}

void HsEventDeathHandler::OnPlayerJustDied(Player* player)
{
    if (!g_HsEnable || !player || !player->IsInWorld())
        return;

    // The two cases DrainPendingDeaths can act on, tested cheaply here so
    // that the realm's most common death by far -- an ungrouped real player
    // -- costs a comparison instead of a queue entry and a lookup. The drain
    // re-checks everything anyway.
    if (!player->GetGroup() && !(Hs_IsBot(player) && EligibleBot(player)))
        return;

    {
        std::lock_guard<std::mutex> lock(g_PendingDeathMutex);
        if (g_PendingDeaths.size() >= kMaxPendingDeaths)
        {
            if (g_HsDebugEnabled)
                LOG_INFO(kHsLogChat,
                    "[HearthsideChat] Pending-death buffer full ({}); dropping this death.", kMaxPendingDeaths);
            return;
        }
        g_PendingDeaths.push_back({ player->GetGUID(), "" });
    }
}

void HsEventKillerHandler::OnPlayerKilledByCreature(Creature* killer, Player* killed)
{
    if (!g_HsEnable || !killer || !killed)
        return;

    // Runs later in the same Unit::Kill that already pushed this player's
    // record microseconds ago, so the match is the last one for that GUID:
    // reverse iteration finds it first. Another map thread may have appended
    // its own death in between, which is exactly why the scan matches on GUID
    // rather than assuming the record is at the back.
    //
    // A death with no record is one OnPlayerJustDied filtered out above, and
    // is simply not ours.
    std::lock_guard<std::mutex> lock(g_PendingDeathMutex);
    for (auto it = g_PendingDeaths.rbegin(); it != g_PendingDeaths.rend(); ++it)
    {
        if (it->playerGuid == killed->GetGUID())
        {
            it->killerName = killer->GetName();
            return;
        }
    }
}

void HsEventDeathDrainWorldScript::OnUpdate(uint32 /*diff*/)
{
    DrainPendingDeaths();
}

// ---- The other five hooks: recorded here, dispatched on the world thread ----
//
// The deferred-death block above found that PlayerScript hooks fire from
// Map::Update on MapUpdate.Threads worker threads (8 on the test realm), and
// that is not special to deaths. A ding comes out of Unit::Kill's XP award,
// a killing blow out of Unit::Kill, a duel's start and end out of
// Player::Update, a roll's award out of the group's loot code. Dispatching
// inline meant FireEvent ran on those threads: a realm-wide
// ObjectAccessor::GetPlayers() walk for the audience and witness scans, and
// topic-gate reads of group members standing on other maps that another
// thread is updating at that moment.
//
// So each hook now captures GUIDs and the strings it will need, applies only
// the cheap tests that read nothing but the hook's own arguments, and hands
// the rest to Hs_DeferToWorldThread (hs_queue.h). World::Update runs the map
// update before OnWorldUpdate, so the reaction is still decided in the same
// tick.
namespace
{
    // There used to be an EventCouldFire() here -- ceiling and budget, tested
    // before a hook deferred anything, so a stream of dings cost nothing once
    // the bucket was empty. Since 2026-09-23 every event also leaves its
    // follow-up fact (HsRecentEvent, above), which needs neither, so the hooks
    // below defer whenever the module is enabled and FireEvent alone decides
    // whether a reaction follows.

    // Guild-scoped events (2026-09-23): every online eligible bot in the
    // guild is a witness and the reaction goes to guild chat. `subject` is the
    // real player it is about, and the origin. Guild chat is its own audience,
    // so the gate is only whether any real member will read it: the subject,
    // while still a member (a login, a join, a ding), or anyone else online.
    //
    // Real players only, at every call site: bots log in, level and earn
    // achievements all day, and a guild of them greeting each other would be
    // the busiest channel on the realm.
    void FireGuildEvent(HsEventType type, Player* subject, uint32 guildId, std::string const& trigger,
                        bool subjectHears, std::vector<Player*> const& exclude = {})
    {
        Guild* guild = guildId ? sGuildMgr->GetGuildById(guildId) : nullptr;
        if (!guild || !subject)
            return;

        std::vector<HsEventActor> actors;
        bool audience = subjectHears;
        auto collect = [&](Player* member)
        {
            if (!member || !member->IsInWorld())
                return;
            if (!Hs_IsBot(member))
            {
                audience = true;
                return;
            }
            if (!EligibleBot(member) || std::find(exclude.begin(), exclude.end(), member) != exclude.end())
                return;
            actors.push_back({ member, HsEventInvolvement::Witness, type, trigger, subject });
        };
        guild->BroadcastWorker(collect, subject);

        if (audience && !actors.empty())
            FireEvent(type, subject, actors, HsReplyChannel::Guild);
    }

    // The bot has just joined a group a real player leads -- the player
    // invited it. The other direction (a player joining a bot's group) stays
    // hs_opener.cpp's corpus opener; this one replaces that opener's
    // bot-joins half (GreetNewMember), so the moment gets one line, not two.
    void FireGroupJoined(Group* group, Player* bot)
    {
        if (!EligibleBot(bot))
            return;
        Player* leader = Hs_FindInWorld(group->GetLeaderGUID());
        if (!leader || leader == bot || Hs_IsBot(leader))
            return;

        std::vector<HsEventActor> actors;
        actors.push_back({ bot, HsEventInvolvement::Subject, HsEventType::GroupJoined,
            "You have just joined " + std::string(leader->GetName()) + "'s group.", leader });
        FireEvent(HsEventType::GroupJoined, leader, actors, GroupChannelFor(group));
    }

    // A boss down. Every bot in the instance is Affected; the origin is a
    // real player there, who is also the audience. On a dungeon's last boss
    // `react` is false: hs_opener.cpp's dungeon-complete opener already
    // speaks for that moment, so this only leaves the follow-up fact.
    void FireBossKilled(std::vector<ObjectGuid> const& botGuids, Player* player, std::string const& bossName, bool react)
    {
        std::string trigger = "Your group has just killed " + bossName + ".";
        std::vector<HsEventActor> actors;
        Group* group = nullptr;
        for (ObjectGuid const& guid : botGuids)
        {
            Player* bot = Hs_FindInWorld(guid);
            if (!bot || !EligibleBot(bot) || !bot->GetGroup())
                continue;
            if (!group)
                group = bot->GetGroup();
            actors.push_back({ bot, HsEventInvolvement::Affected, HsEventType::BossKilled, trigger });
        }
        if (actors.empty())
            return;

        if (!react)
        {
            for (auto const& actor : actors)
                NoteRecentEvent(actor.bot, actor.trigger, nullptr);
            return;
        }
        FireEvent(HsEventType::BossKilled, player, actors, GroupChannelFor(group));
    }

    // A real player has just brought this bot back. Group channel when the
    // two are grouped, /say otherwise (a rez is cast from 30 yards, so the
    // rezzer is always in range to read it).
    void FireResurrected(Player* bot, Player* rezzer)
    {
        if (!EligibleBot(bot))
            return;
        std::vector<HsEventActor> actors;
        actors.push_back({ bot, HsEventInvolvement::Subject, HsEventType::Resurrected,
            std::string(rezzer->GetName()) + " has just resurrected you.", rezzer });

        Group* group = bot->GetGroup();
        bool together = group && group == rezzer->GetGroup();
        FireEvent(HsEventType::Resurrected, rezzer, actors, together ? GroupChannelFor(group) : HsReplyChannel::Say);
    }

    void FireTradeOpened(Player* bot, Player* player)
    {
        if (!EligibleBot(bot))
            return;
        std::vector<HsEventActor> actors;
        actors.push_back({ bot, HsEventInvolvement::Subject, HsEventType::TradeOpened,
            std::string(player->GetName()) + " has just opened a trade with you.", player });
        FireEvent(HsEventType::TradeOpened, player, actors, HsReplyChannel::Say);
    }

    // ---- Trade completion ---------------------------------------------------
    // AzerothCore has no "trade went through" hook. What it does have is the
    // two things a completed trade does while both sides' TradeData are still
    // attached (WorldSession::HandleAcceptTradeOpcode, TradeHandler.cpp):
    // moveItems -> Player::MoveItemToInventory -> OnPlayerAfterMoveItemToInventory
    // for each item, then Player::ModifyMoney -> OnPlayerMoneyChanged for the
    // gold, all before `m_trade` is deleted. So each of those calls, seen on a
    // player with trade data and a trader, is one piece of one trade. Read
    // from the core source 2026-09-23, not yet seen firing on the realm.
    //
    // They arrive as up to seven separate calls in one call stack, so the
    // pieces are collected per bot and the first one defers a single flush:
    // by the time the world thread runs it, the trade has finished.
    //
    // Item names only, no counts: an item that merges into an existing stack
    // reaches the hook as the merged stack (TradeHandler.cpp says as much), so
    // the count there is the bot's bag, not what was traded.
    struct HsPendingTrade
    {
        ObjectGuid               player;
        std::vector<std::string> toBot;
        std::vector<std::string> fromBot;
        uint32                   copperToBot   = 0;
        uint32                   copperFromBot = 0;
    };

    std::mutex                                   g_PendingTradeMutex;
    std::unordered_map<uint64_t, HsPendingTrade> g_PendingTrades;

    std::string MoneyText(uint32 copper)
    {
        if (copper >= 10000)
            return std::to_string(copper / 10000) + " gold";
        if (copper >= 100)
            return std::to_string(copper / 100) + " silver";
        return std::to_string(copper) + " copper";
    }

    // "a", "a and b", "a, b and c".
    std::string JoinThings(std::vector<std::string> const& things)
    {
        std::string out;
        for (size_t i = 0; i < things.size(); ++i)
        {
            if (i > 0)
                out += (i + 1 == things.size()) ? " and " : ", ";
            out += things[i];
        }
        return out;
    }

    void FlushTrade(ObjectGuid botGuid)
    {
        HsPendingTrade trade;
        {
            std::lock_guard<std::mutex> lock(g_PendingTradeMutex);
            auto it = g_PendingTrades.find(botGuid.GetRawValue());
            if (it == g_PendingTrades.end())
                return;
            trade = std::move(it->second);
            g_PendingTrades.erase(it);
        }

        Player* bot    = Hs_FindInWorld(botGuid);
        Player* player = Hs_FindInWorld(trade.player);
        if (!bot || !player || !EligibleBot(bot))
            return;

        // What the bot received decides the line when both sides gave
        // something: a player buying from a bot paid it, and "given you 5
        // gold" is the half the bot has an opinion about.
        std::vector<std::string> received = trade.toBot;
        if (trade.copperToBot)
            received.push_back(MoneyText(trade.copperToBot));
        std::vector<std::string> gave = trade.fromBot;
        if (trade.copperFromBot)
            gave.push_back(MoneyText(trade.copperFromBot));

        std::string trigger;
        if (!received.empty())
            trigger = std::string(player->GetName()) + " has just given you " + JoinThings(received) + ".";
        else if (!gave.empty())
            trigger = "You have just given " + std::string(player->GetName()) + " " + JoinThings(gave) + ".";
        else
            return;

        std::vector<HsEventActor> actors;
        actors.push_back({ bot, HsEventInvolvement::Subject, HsEventType::TradeCompleted, trigger, player });
        FireEvent(HsEventType::TradeCompleted, player, actors, HsReplyChannel::Say);
    }

    // One piece of a trade, seen from the side that just received it. Only a
    // bot-and-real-player trade counts, in either direction. Called from map
    // and world threads alike, so it reads nothing but the two players' own
    // GUIDs and bot-ness before handing off.
    void NoteTradePiece(Player* receiver, std::string const& itemName, uint32 copper)
    {
        Player* giver = receiver ? receiver->GetTrader() : nullptr;
        if (!giver)
            return;
        bool toBot = Hs_IsBot(receiver);
        Player* bot   = toBot ? receiver : giver;
        Player* human = toBot ? giver : receiver;
        if (!Hs_IsBot(bot) || Hs_IsBot(human))
            return;

        ObjectGuid botGuid = bot->GetGUID();
        bool first = false;
        {
            std::lock_guard<std::mutex> lock(g_PendingTradeMutex);
            auto [it, inserted] = g_PendingTrades.try_emplace(botGuid.GetRawValue());
            HsPendingTrade& trade = it->second;
            first = inserted;
            trade.player = human->GetGUID();
            std::vector<std::string>& things = toBot ? trade.toBot : trade.fromBot;
            if (!itemName.empty() && std::find(things.begin(), things.end(), itemName) == things.end())
                things.push_back(itemName);
            (toBot ? trade.copperToBot : trade.copperFromBot) += copper;
        }
        if (first)
            Hs_DeferToWorldThread([botGuid]() { FlushTrade(botGuid); });
    }

    // The bot's own achievement: guild chat when it is in a guild a real
    // player can read (the guild already sees the toast), its group when
    // grouped, /say otherwise.
    void FireAchievementSelf(Player* bot, std::string const& name)
    {
        if (!EligibleBot(bot))
            return;
        std::vector<HsEventActor> actors;
        actors.push_back({ bot, HsEventInvolvement::Subject, HsEventType::AchievementSelf,
            "You have just earned the achievement " + name + "." });

        HsReplyChannel channel = HsReplyChannel::Say;
        bool guildAudience = false;
        if (Guild* guild = bot->GetGuildId() ? sGuildMgr->GetGuildById(bot->GetGuildId()) : nullptr)
        {
            auto anyReal = [&guildAudience](Player* member)
            {
                if (member && member->IsInWorld() && !Hs_IsBot(member))
                    guildAudience = true;
            };
            guild->BroadcastWorker(anyReal, bot);
        }
        if (guildAudience)
            channel = HsReplyChannel::Guild;
        else if (Group* group = bot->GetGroup())
            channel = GroupChannelFor(group);
        FireEvent(HsEventType::AchievementSelf, bot, actors, channel);
    }

    // One side of a finished battleground or arena match. `realPlayer` is a
    // real player on that side, the origin and the audience; a side with no
    // real player is never passed in, since nobody who could read the line
    // is there to read it.
    void FireMatchEnd(std::vector<ObjectGuid> const& botGuids, Player* realPlayer, bool won, bool isArena,
                      std::string const& trigger)
    {
        HsEventType type = isArena ? (won ? HsEventType::ArenaWon : HsEventType::ArenaLost)
                                   : (won ? HsEventType::BattlegroundWon : HsEventType::BattlegroundLost);
        std::vector<HsEventActor> actors;
        Group* group = nullptr;
        for (ObjectGuid const& guid : botGuids)
        {
            Player* bot = Hs_FindInWorld(guid);
            if (!bot || !EligibleBot(bot))
                continue;
            if (!group)
                group = bot->GetGroup();
            actors.push_back({ bot, HsEventInvolvement::Affected, type, trigger });
        }
        if (actors.empty())
            return;
        FireEvent(type, realPlayer, actors, group ? GroupChannelFor(group) : HsReplyChannel::Say);
    }

    void FireLevelUp(Player* player, uint8 newLevel)
    {
        std::string levelText = std::to_string(static_cast<uint32_t>(newLevel));
        std::vector<HsEventActor> actors;

        // The one who dinged speaks in second person, everyone else in third:
        // one arbitration over the combined pool, each side carrying its own
        // trigger and its own affinity type (the same shape the duel end uses).
        if (Hs_IsBot(player) && EligibleBot(player))
        {
            actors.push_back({ player, HsEventInvolvement::Subject, HsEventType::LevelUpSelf,
                "You have just reached level " + levelText + "." });
        }

        std::string witnessTrigger = std::string(player->GetName()) +
            " has just reached level " + levelText + ".";

        Group* group = player->GetGroup();
        if (group)
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (!member || member == player || !EligibleBot(member))
                    continue;
                actors.push_back({ member, HsEventInvolvement::Witness, HsEventType::LevelUpGroup, witnessTrigger, player });
            }
        }
        else
        {
            // Review item 14: the budget gate lives inside FireEvent, which is
            // called at the bottom of this function -- so this scan (a realm
            // walk with a distance check per candidate, NearbyBots above) ran
            // in full and had its result discarded every time the bucket was
            // already empty. It still skips the scan then, but only for a bot's
            // ding: the witnesses of a real player's ding also get the
            // follow-up fact, for the "ding!" that player is about to type.
            //
            // World-scoped: a ding in the open is worth a "gz" from whoever is
            // standing there, real player present or not.
            if (!Hs_EventBucketExhausted() || !Hs_IsBot(player))
                for (Player* nearby : NearbyBots(player, nullptr))
                    actors.push_back({ nearby, HsEventInvolvement::Witness, HsEventType::LevelUpGroup, witnessTrigger, player });
        }

        // LEVEL_UP_SELF drives the reply count when the bot itself dinged, since
        // that is the more constrained draw; a real player's ding falls back to
        // the witness bias, which is the one that almost always produces a "gz".
        HsEventType primary = (Hs_IsBot(player) && EligibleBot(player))
            ? HsEventType::LevelUpSelf : HsEventType::LevelUpGroup;

        FireEvent(primary, player, actors, group ? GroupChannelFor(group) : HsReplyChannel::Say);

        // And the guild, for a real player's ding: guild chat is where a "gz"
        // for someone who is not standing next to you happens. A separate
        // event with its own token and its own channel; bots that already had
        // the group or /say trigger above are left out so none gets both.
        if (!Hs_IsBot(player) && player->GetGuildId())
        {
            std::vector<Player*> already;
            for (auto const& actor : actors)
                already.push_back(actor.bot);
            FireGuildEvent(HsEventType::GuildLevelUp, player, player->GetGuildId(),
                std::string(player->GetName()) + ", in your guild, has just reached level " + levelText + ".",
                /*subjectHears=*/true, already);
        }
    }

    void FirePvpKill(Player* killer, std::string const& killedName)
    {
        if (!EligibleBot(killer))
            return;

        std::vector<HsEventActor> actors;
        actors.push_back({ killer, HsEventInvolvement::Subject, HsEventType::KillingBlow,
            "You have just killed " + killedName + " in a fight." });

        // Spoken to the killer's own side. The victim is cross-faction and
        // cannot read it either way; the audience is whoever shares the killer's
        // group, or /say range if it has none.
        Group* group = killer->GetGroup();
        FireEvent(HsEventType::KillingBlow, killer, actors,
            group ? GroupChannelFor(group) : HsReplyChannel::Say);
    }

    void FireRollWon(Player* player, std::string const& itemName, std::vector<ObjectGuid> const& rollers)
    {
        Group* group = player->GetGroup();
        if (!group)
            return;

        std::vector<HsEventActor> actors;

        if (EligibleBot(player))
        {
            actors.push_back({ player, HsEventInvolvement::Subject, HsEventType::RollWon,
                "You have just won the roll for " + itemName + "." });
        }

        std::string lostTrigger = std::string(player->GetName()) +
            " has just won the roll for " + itemName + ".";
        for (ObjectGuid const& rollerGuid : rollers)
        {
            Player* loser = Hs_FindInWorld(rollerGuid);
            if (!loser || !EligibleBot(loser))
                continue;
            actors.push_back({ loser, HsEventInvolvement::Affected, HsEventType::RollLost, lostTrigger, player });
        }

        HsEventType primary = EligibleBot(player) ? HsEventType::RollWon : HsEventType::RollLost;
        FireEvent(primary, player, actors, GroupChannelFor(group));
    }

    void FireDuelStart(Player* player1, Player* player2)
    {
        std::vector<HsEventActor> actors;
        auto addSide = [&actors](Player* bot, Player* opponent)
        {
            if (!EligibleBot(bot))
                return;
            actors.push_back({ bot, HsEventInvolvement::Subject, HsEventType::DuelStart,
                "A duel between you and " + std::string(opponent->GetName()) + " is starting.", opponent });
        };
        addSide(player1, player2);
        addSide(player2, player1);

        // The two participants are the only valid speakers: a bystander
        // commenting on someone else's duel is not one of the sixteen triggers.
        FireEvent(HsEventType::DuelStart, player1, actors, HsReplyChannel::Say);
    }

    void FireDuelEnd(Player* winner, Player* loser)
    {
        std::vector<HsEventActor> actors;

        // One pass over {winner, loser}, each carrying its own outcome. Which
        // trigger text reaches Hs_CallLLM is therefore decided by which side the
        // arbiter picks, not fixed here, and each side's affinity resolves
        // against its own event type, so an archetype can be eager to gloat and
        // reluctant to admit a loss (Claude/archive/PLAN-ARBITER.md §2).
        if (EligibleBot(winner))
        {
            actors.push_back({ winner, HsEventInvolvement::Subject, HsEventType::DuelWon,
                "You have just won a duel against " + std::string(loser->GetName()) + ".", loser });
        }
        if (EligibleBot(loser))
        {
            actors.push_back({ loser, HsEventInvolvement::Subject, HsEventType::DuelLost,
                "You have just lost a duel to " + std::string(winner->GetName()) + ".", winner });
        }

        // DUEL_WON supplies the count bias for the combined pass; both duel
        // outcomes carry the same heavy bias toward silence, so which one is
        // read here does not change the distribution.
        FireEvent(HsEventType::DuelWon, winner, actors, HsReplyChannel::Say);
    }
}

void HsEventLevelHandler::OnPlayerLevelChanged(Player* player, uint8 oldlevel)
{
    if (!g_HsEnable || !player || !player->IsInWorld())
        return;

    // The hook fires on any change; a GM down-level is not a ding. There is
    // no way to tell an organic ding from a RandomPlayerbotMgr bracket
    // relevel through this signature, and that noise is accepted
    // (Claude/archive/PLAN-ARBITER.md §3).
    uint8 newLevel = player->GetLevel();
    if (newLevel <= oldlevel)
    {
        // A *drop* is how mod-playerbots' recycler shows up here:
        // RandomBotLevelMgr::ResetBot knocks a bot back down a bracket in
        // place, keeping its GUID and name. Nothing about the character it
        // was survives that, so its conversation history stops being useful
        // prior-turn context and starts being wrong. This hook is the only
        // signal that covers every bot: hside_identity's own retirement
        // sweep only walks carded rows. Every map it clears is under its own
        // mutex, so this one runs inline.
        if (newLevel < oldlevel && Hs_IsBot(player))
            Hs_ForgetBotHistory(player->GetGUID().GetRawValue());
        return;
    }

    // newLevel is captured, not re-read at dispatch: a quest turn-in worth
    // two levels fires this hook twice, and each ding says its own number.
    ObjectGuid guid = player->GetGUID();
    Hs_DeferToWorldThread([guid, newLevel]()
    {
        if (Player* dinged = Hs_FindInWorld(guid))
            FireLevelUp(dinged, newLevel);
    });
}

void HsEventPvpKillHandler::OnPlayerPVPKill(Player* killer, Player* killed)
{
    if (!g_HsEnable || !killer || !killed)
        return;

    // The core reaches this hook with killer == killed often enough to
    // matter: 18 of ~40 killing-blow rows in hside_chat_log on 2026-09-13
    // were a bot told it had killed itself ("You have just killed
    // Azaedrine in a fight." delivered to Azaedrine), which is the single
    // most obviously-fake line the module has produced. Guarded on GUID
    // rather than pointer identity so a re-resolved Player* for the same
    // character is caught too.
    if (killer->GetGUID() == killed->GetGUID())
        return;

    if (!Hs_IsBot(killer))
        return;

    // The victim's name, captured now: the killing blow is the moment it is
    // true, and the victim may release and leave before the drain.
    ObjectGuid  killerGuid = killer->GetGUID();
    std::string killedName = killed->GetName();
    Hs_DeferToWorldThread([killerGuid, killedName]()
    {
        if (Player* bot = Hs_FindInWorld(killerGuid))
            FirePvpKill(bot, killedName);
    });
}

void HsEventRollHandler::OnPlayerGroupRollRewardItem(Player* player, Item* item, uint32 /*count*/,
                                                      RollVote /*voteType*/, Roll* roll)
{
    if (!g_HsEnable || !player || !item || !roll)
        return;

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return;
    // A group roll happens on greens too; without this the pair is noise.
    if (proto->Quality < ITEM_QUALITY_RARE)
        return;

    if (!player->GetGroup())
        return;

    std::string itemName = Hs_LocalizedItemName(proto); // review H1
    // Review item 8: Hs_LocalizedItemName returns "" for a template whose
    // name resolves empty, not only for a null one. Both trigger strings
    // interpolate it into a sentence ("...won the roll for ."), and that
    // sentence is fed verbatim into a bot-to-bot reaction prompt. An item
    // nobody can name is not worth an event.
    if (itemName.empty())
        return;

    // Only bots that actually rolled on it lost anything. A bot that passed
    // has nothing to react to, and treating it as a candidate is the kind of
    // state the trigger never established (Claude/archive/PLAN-ARBITER.md §7
    // rule 2). Read out of the Roll now: it does not outlive the award.
    std::vector<ObjectGuid> rollers;
    for (auto const& vote : roll->playerVote)
    {
        if (vote.first == player->GetGUID())
            continue;
        if (vote.second == NEED || vote.second == GREED)
            rollers.push_back(vote.first);
    }

    ObjectGuid winnerGuid = player->GetGUID();
    Hs_DeferToWorldThread([winnerGuid, itemName, rollers]()
    {
        if (Player* winner = Hs_FindInWorld(winnerGuid))
            FireRollWon(winner, itemName, rollers);
    });
}

void HsEventDuelHandler::OnPlayerDuelStart(Player* player1, Player* player2)
{
    if (!g_HsEnable || !player1 || !player2)
        return;

    ObjectGuid guid1 = player1->GetGUID();
    ObjectGuid guid2 = player2->GetGUID();
    Hs_DeferToWorldThread([guid1, guid2]()
    {
        Player* first  = Hs_FindInWorld(guid1);
        Player* second = Hs_FindInWorld(guid2);
        if (first && second)
            FireDuelStart(first, second);
    });
}

void HsEventDuelHandler::OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type)
{
    if (!g_HsEnable || !winner || !loser)
        return;
    if (type == DUEL_INTERRUPTED)
        return; // nobody won; there is no outcome to react to

    ObjectGuid winnerGuid = winner->GetGUID();
    ObjectGuid loserGuid  = loser->GetGUID();
    Hs_DeferToWorldThread([winnerGuid, loserGuid]()
    {
        Player* won  = Hs_FindInWorld(winnerGuid);
        Player* lost = Hs_FindInWorld(loserGuid);
        if (won && lost)
            FireDuelEnd(won, lost);
    });
}

// ---- The 2026-09-23 hooks: same shape as the five above ---------------------

void HsEventGroupJoinHandler::OnAddMember(Group* group, ObjectGuid guid)
{
    if (!g_HsEnable || !group)
        return;

    // By GUID, as hs_opener.cpp's handler for the same hook does: the group
    // can be disbanded before the world thread gets here.
    ObjectGuid::LowType groupId = group->GetGUID().GetCounter();
    Hs_DeferToWorldThread([groupId, guid]()
    {
        Group*  live = sGroupMgr->GetGroupByGUID(groupId);
        Player* bot  = Hs_FindInWorld(guid);
        if (live && bot && Hs_IsBot(bot))
            FireGroupJoined(live, bot);
    });
}

void HsEventEncounterHandler::OnAfterUpdateEncounterState(Map* map, EncounterCreditType type, uint32_t creditEntry,
                                                            Unit* /*source*/, Difficulty /*difficultyFixed*/,
                                                            DungeonEncounterList const* encounters,
                                                            uint32_t dungeonCompleted, bool updated)
{
    // `updated` is Map::UpdateEncounterState's "this credit newly completed
    // an encounter" (the instance's completed mask changed): the hook itself
    // also fires for credits that match nothing, and again for a boss that
    // was already down. An instance with no InstanceScript never sets it and
    // so never fires this -- accepted, since those are the instances with no
    // encounter bookkeeping to trust anyway.
    if (!g_HsEnable || !map || !encounters || !updated)
        return;

    std::string bossName;
    for (DungeonEncounter const* encounter : *encounters)
    {
        if (encounter && encounter->creditType == type && encounter->creditEntry == creditEntry)
        {
            bossName = Hs_LocalizedEncounterName(encounter->dbcEntry);
            break;
        }
    }
    if (bossName.empty())
        return;

    // The instance's own player list, walked on the thread updating this map
    // (the same thing hs_opener.cpp's dungeon-complete handler does here).
    std::vector<ObjectGuid> botGuids;
    ObjectGuid              playerGuid;
    Map::PlayerList const& players = map->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player* member = itr->GetSource();
        if (!member || !member->IsInWorld())
            continue;
        if (Hs_IsBot(member))
            botGuids.push_back(member->GetGUID());
        else if (!playerGuid)
            playerGuid = member->GetGUID();
    }
    if (botGuids.empty() || !playerGuid)
        return;

    bool react = (dungeonCompleted == 0);
    Hs_DeferToWorldThread([botGuids, playerGuid, bossName, react]()
    {
        if (Player* player = Hs_FindInWorld(playerGuid))
            FireBossKilled(botGuids, player, bossName, react);
    });
}

Player* Hs_RealPlayerResurrecting(Player* bot)
{
    // Player keeps the rezzer's GUID private (m_resurrectGUID) and exposes
    // only isResurrectRequestedBy(guid), so the real players on the bot's map
    // are asked one by one. Set by the resurrect spell effect
    // (SpellEffects.cpp) and still set when the bot accepts, since only dying
    // clears it; a spirit healer or graveyard rez sets nothing, and reads as
    // no rezzer at all.
    if (!bot || !bot->isResurrectRequested())
        return nullptr;
    Map* map = bot->GetMap();
    if (!map)
        return nullptr;
    Map::PlayerList const& players = map->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (candidate && candidate != bot && candidate->IsInWorld() && !Hs_IsBot(candidate)
            && bot->isResurrectRequestedBy(candidate->GetGUID()))
            return candidate;
    }
    return nullptr;
}

void HsEventResurrectHandler::OnPlayerResurrect(Player* player, float /*restorePercent*/, bool& /*applySickness*/)
{
    if (!g_HsEnable || !player || !Hs_IsBot(player))
        return;

    Player* rezzer = Hs_RealPlayerResurrecting(player);
    if (!rezzer)
        return;

    ObjectGuid botGuid    = player->GetGUID();
    ObjectGuid rezzerGuid = rezzer->GetGUID();
    Hs_DeferToWorldThread([botGuid, rezzerGuid]()
    {
        Player* bot  = Hs_FindInWorld(botGuid);
        Player* live = Hs_FindInWorld(rezzerGuid);
        if (bot && live)
            FireResurrected(bot, live);
    });
}

bool HsEventTradeHandler::OnPlayerCanInitTrade(Player* player, Player* target)
{
    // A real player opening a trade with a bot. Never vetoes: this is an
    // observer on a permission hook.
    if (g_HsEnable && player && target && !Hs_IsBot(player) && Hs_IsBot(target))
    {
        ObjectGuid botGuid    = target->GetGUID();
        ObjectGuid playerGuid = player->GetGUID();
        Hs_DeferToWorldThread([botGuid, playerGuid]()
        {
            Player* bot  = Hs_FindInWorld(botGuid);
            Player* live = Hs_FindInWorld(playerGuid);
            if (bot && live)
                FireTradeOpened(bot, live);
        });
    }
    return true;
}

void HsEventTradeHandler::OnPlayerMoneyChanged(Player* player, int32& amount)
{
    // Positive for the side receiving. Everything else that moves money
    // (loot, vendors, repairs) happens with no trade attached and stops here.
    if (!g_HsEnable || !player || amount <= 0 || !player->GetTradeData())
        return;
    NoteTradePiece(player, "", static_cast<uint32>(amount));
}

void HsEventTradeHandler::OnPlayerAfterMoveItemToInventory(Player* player, Item* item, bool /*update*/)
{
    if (!g_HsEnable || !player || !item || !player->GetTradeData())
        return;
    NoteTradePiece(player, Hs_LocalizedItemName(item->GetTemplate()), 0);
}

void HsEventLoginHandler::OnPlayerLogin(Player* player)
{
    if (!g_HsEnable || !player || Hs_IsBot(player) || !player->GetGuildId())
        return;

    ObjectGuid guid    = player->GetGUID();
    uint32     guildId = player->GetGuildId();
    Hs_DeferToWorldThread([guid, guildId]()
    {
        if (Player* live = Hs_FindInWorld(guid))
            FireGuildEvent(HsEventType::GuildLogin, live, guildId,
                std::string(live->GetName()) + ", in your guild, has just logged in.", /*subjectHears=*/true);
    });
}

void HsEventAchievementHandler::OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement)
{
    if (!g_HsEnable || !player || !achievement)
        return;
    // Statistics are counters that "complete" as they tick; they are not
    // something anyone congratulates.
    if (achievement->flags & ACHIEVEMENT_FLAG_COUNTER)
        return;
    std::string name = Hs_LocalizedAchievementName(achievement);
    if (name.empty())
        return;

    bool       isBot   = Hs_IsBot(player);
    uint32     guildId = player->GetGuildId();
    if (!isBot && !guildId)
        return;

    ObjectGuid guid = player->GetGUID();
    Hs_DeferToWorldThread([guid, guildId, isBot, name]()
    {
        Player* live = Hs_FindInWorld(guid);
        if (!live)
            return;
        if (isBot)
            FireAchievementSelf(live, name);
        else
            FireGuildEvent(HsEventType::AchievementGuild, live, guildId,
                std::string(live->GetName()) + ", in your guild, has just earned the achievement " + name + ".",
                /*subjectHears=*/true);
    });
}

void HsEventGuildHandler::OnAddMember(Guild* guild, Player* player, uint8& /*plRank*/)
{
    // `player` is null when the member was added offline (Guild::AddMember).
    if (!g_HsEnable || !guild || !player || Hs_IsBot(player))
        return;

    uint32     guildId = guild->GetId();
    ObjectGuid guid    = player->GetGUID();
    Hs_DeferToWorldThread([guildId, guid]()
    {
        if (Player* live = Hs_FindInWorld(guid))
            FireGuildEvent(HsEventType::GuildJoined, live, guildId,
                std::string(live->GetName()) + " has just joined your guild.", /*subjectHears=*/true);
    });
}

void HsEventGuildHandler::OnRemoveMember(Guild* guild, Player* player, bool isDisbanding, bool isKicked)
{
    if (!g_HsEnable || !guild || !player || isDisbanding || Hs_IsBot(player))
        return;

    // Called before the member is actually removed (Guild::DeleteMember);
    // by the time the world thread runs this the player is out of guild
    // chat, so someone else has to be there to read it.
    uint32     guildId = guild->GetId();
    ObjectGuid guid    = player->GetGUID();
    Hs_DeferToWorldThread([guildId, guid, isKicked]()
    {
        Player* live = Hs_FindInWorld(guid);
        if (!live)
            return;
        std::string name = live->GetName();
        FireGuildEvent(HsEventType::GuildLeft, live, guildId,
            isKicked ? name + " has just been removed from your guild." : name + " has just left your guild.",
            /*subjectHears=*/false);
    });
}

void HsEventBattlegroundHandler::OnBattlegroundEnd(Battleground* bg, TeamId winnerTeamId)
{
    if (!g_HsEnable || !bg || winnerTeamId == TEAM_NEUTRAL)
        return; // a draw has no side to be on

    // Both sides at once, read off the battleground's own player map on the
    // thread ending it; one real player per side becomes that side's origin.
    // Arena::EndBattleground ends in Battleground::EndBattleground, so arenas
    // arrive here too.
    std::vector<ObjectGuid> bots[2];
    ObjectGuid              realPlayer[2];
    for (auto const& [guid, player] : bg->GetPlayers())
    {
        if (!player)
            continue;
        int side = (player->GetBgTeamId() == winnerTeamId) ? 0 : 1;
        if (Hs_IsBot(player))
            bots[side].push_back(guid);
        else if (!realPlayer[side])
            realPlayer[side] = guid;
    }

    bool        isArena = bg->isArena();
    std::string wonLine, lostLine;
    if (isArena)
    {
        std::string size = std::to_string(bg->GetArenaType());
        wonLine  = "Your team has just won a " + size + "v" + size + " arena match.";
        lostLine = "Your team has just lost a " + size + "v" + size + " arena match.";
    }
    else
    {
        wonLine  = "Your side has just won " + bg->GetName() + ".";
        lostLine = "Your side has just lost " + bg->GetName() + ".";
    }

    for (int side = 0; side < 2; ++side)
    {
        if (bots[side].empty() || !realPlayer[side])
            continue;
        bool               won     = (side == 0);
        std::string        trigger = won ? wonLine : lostLine;
        std::vector<ObjectGuid> sideBots = bots[side];
        ObjectGuid         origin  = realPlayer[side];
        Hs_DeferToWorldThread([sideBots, origin, won, isArena, trigger]()
        {
            if (Player* live = Hs_FindInWorld(origin))
                FireMatchEnd(sideBots, live, won, isArena, trigger);
        });
    }
}

std::string Hs_RecentEventContext(uint64_t botGuid, uint64_t senderGuid)
{
    std::lock_guard<std::mutex> lock(g_RecentEventMutex);
    auto it = g_RecentEvents.find(botGuid);
    if (it == g_RecentEvents.end())
        return "";

    HsRecentEvent const& recent = it->second;
    auto age = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - recent.at).count();
    if (age > kRecentEventWindowSeconds)
        return "";

    // The model is never told who is speaking to it, so a winner's "loser"
    // and a bystander's read the same. When they are the other person in the
    // event, it says so.
    std::string out = recent.line;
    if (senderGuid && senderGuid == recent.counterpartGuid && !recent.counterpartName.empty())
        out += "\nThe one talking to you now is " + recent.counterpartName + ".";
    return out;
}

uint32_t Hs_EventsFiredThisSession()
{
    return g_EventsFiredThisSession.load();
}
