#include "hs_event.h"
#include "hs_archetype.h"
#include "hs_bot.h"
#include "hs_config.h"
#include "hs_event_arbiter.h"
#include "hs_log.h"
#include "hs_queue.h"
#include "hs_tier.h"
#include "hs_locale.h"

#include "Creature.h"
#include "Group.h"
#include "GroupReference.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SharedDefines.h"

#include <atomic>
#include <mutex>
#include <string>
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
    struct HsEventActor
    {
        Player*            bot;
        HsEventInvolvement involvement;
        HsEventType        affinityType;
        std::string        trigger;
    };

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
        candidates.reserve(actors.size());
        bots.reserve(actors.size());

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

            HsReplyRequest request = Hs_MakeReplyRequest(bot, origin, channel, candidates[index].trigger);
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
            if (!candidate || candidate == origin || !candidate->IsInWorld())
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
                actors.push_back({ member, HsEventInvolvement::Affected, HsEventType::DeathGroupPlayer, trigger });
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
    // Ceiling and budget, both scalars or locked, so safe from any thread:
    // a stream of dings or kills costs a queue entry only when a reaction
    // could still happen. FireEvent checks both again for real.
    bool EventCouldFire()
    {
        return HsTierAllows(g_HsMaxTierEvents, HsTier::Inference) && !Hs_EventBucketExhausted();
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
                actors.push_back({ member, HsEventInvolvement::Witness, HsEventType::LevelUpGroup, witnessTrigger });
            }
        }
        else
        {
            // Review item 14: the budget gate lives inside FireEvent, which is
            // called at the bottom of this function -- so this scan (a realm
            // walk with a distance check per candidate, NearbyBots above) ran
            // in full and had its result discarded every time the bucket was
            // already empty. Returning rather than skipping just the scan:
            // with no budget FireEvent would drop the event regardless of
            // which actors were collected.
            if (Hs_EventBucketExhausted())
                return;

            // World-scoped: a ding in the open is worth a "gz" from whoever is
            // standing there, real player present or not.
            for (Player* nearby : NearbyBots(player, nullptr))
                actors.push_back({ nearby, HsEventInvolvement::Witness, HsEventType::LevelUpGroup, witnessTrigger });
        }

        // LEVEL_UP_SELF drives the reply count when the bot itself dinged, since
        // that is the more constrained draw; a real player's ding falls back to
        // the witness bias, which is the one that almost always produces a "gz".
        HsEventType primary = (Hs_IsBot(player) && EligibleBot(player))
            ? HsEventType::LevelUpSelf : HsEventType::LevelUpGroup;

        FireEvent(primary, player, actors, group ? GroupChannelFor(group) : HsReplyChannel::Say);
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
            actors.push_back({ loser, HsEventInvolvement::Affected, HsEventType::RollLost, lostTrigger });
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
                "A duel between you and " + std::string(opponent->GetName()) + " is starting." });
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
                "You have just won a duel against " + std::string(loser->GetName()) + "." });
        }
        if (EligibleBot(loser))
        {
            actors.push_back({ loser, HsEventInvolvement::Subject, HsEventType::DuelLost,
                "You have just lost a duel to " + std::string(winner->GetName()) + "." });
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

    if (!EventCouldFire())
        return;

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

    if (!Hs_IsBot(killer) || !EventCouldFire())
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

    if (!player->GetGroup() || !EventCouldFire())
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
    if (!g_HsEnable || !player1 || !player2 || !EventCouldFire())
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
    if (!EventCouldFire())
        return;

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

uint32_t Hs_EventsFiredThisSession()
{
    return g_EventsFiredThisSession.load();
}
