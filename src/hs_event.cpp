#include "hs_event.h"
#include "hs_archetype.h"
#include "hs_bot.h"
#include "hs_config.h"
#include "hs_corpus.h"
#include "hs_event_arbiter.h"
#include "hs_log.h"
#include "hs_queue.h"
#include "hs_tier.h"
#include "hs_locale.h"
#include "hs_prune.h"
#include "hs_style.h"

#include "Battleground.h"
#include "BattlegroundAB.h"
#include "BattlegroundAV.h"
#include "BattlegroundEY.h"
#include "BattlegroundIC.h"
#include "BattlegroundWS.h"
#include "Creature.h"
#include "GameTime.h"
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
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "AiObjectContext.h"
#include "PositionValue.h"
#include "Random.h"
#include "SharedDefines.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>
#include <cctype>
#include <chrono>
#include <iterator>
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
        HsReplyChannel                        channel = HsReplyChannel::Say; // where the event was spoken
        std::chrono::steady_clock::time_point at;
        bool                                  duelEnd  = false; // won or lost a duel against the counterpart
        bool                                  answered = false; // that duel's one comeback is spent (Hs_MaybeAnswerDuelLine)
    };

    // Two minutes: long enough to cover "gg" typed after a duel or a "wb"
    // after a login, short enough that "has just" is still true. A starting
    // judgement, same status as the bias table.
    constexpr int64_t kRecentEventWindowSeconds  = 120;
    constexpr size_t  kRecentEventPruneThreshold = 512;

    std::mutex                                  g_RecentEventMutex;
    std::unordered_map<uint64_t, HsRecentEvent> g_RecentEvents;

    void NoteRecentEvent(Player* bot, std::string const& line, Player* counterpart, HsReplyChannel channel,
                         bool duelEnd = false)
    {
        HsRecentEvent entry;
        entry.line = line;
        entry.channel = channel;
        entry.duelEnd = duelEnd;
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

    // ---- One reaction per kind of event, per bot, per ten minutes -----------
    //
    // A bot that keeps dying near a player reacted to every death with the
    // same prompt, and at temperature 0.5 the same prompt is the same line:
    // "oh no, that wasnt me" three times in twenty minutes (realm
    // 2026-10-09). A person who dies six times in a row stops narrating it.
    // Admission only, like the event bucket: nothing about the reply is
    // checked. The bot still gets the recent-event line, so a player who
    // talks to it about the death gets an answer.
    constexpr int64_t kSameEventCooldownSeconds = 600;

    HsCooldownMap<uint64_t> g_ReactionCooldown; // keyed (botGuid << 8) | event type

    bool ReactedRecently(uint64_t botGuid, HsEventType type)
    {
        return !g_ReactionCooldown.Ready((botGuid << 8) | static_cast<uint8_t>(type), kSameEventCooldownSeconds);
    }

    void NoteReaction(uint64_t botGuid, HsEventType type)
    {
        g_ReactionCooldown.Mark((botGuid << 8) | static_cast<uint8_t>(type), kSameEventCooldownSeconds);
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
    std::string GuildLoginGreeting(std::string name)
    {
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        static const char* const kForms[] = { "hey %", "o/", "yo %", "hi %", "wb", "hey", "o/ %", "sup %", "wb %", "heya" };
        std::string form = kForms[urand(0, std::size(kForms) - 1)];
        size_t at = form.find('%');
        if (at != std::string::npos)
            form.replace(at, 1, name);
        return form;
    }

    void FireEvent(HsEventType primaryType, Player* origin, std::vector<HsEventActor>& actors, HsReplyChannel channel)
    {
        if (!g_HsEnable || !origin || !origin->IsInWorld() || actors.empty())
            return;

        // In a battleground or arena only the match talks: its result here,
        // its progress in OnBattlegroundUpdate. Joining the battleground's raid
        // read as joining a party ("hey, where are we going"), and achievements
        // pop every few seconds there (realm 2026-10-14).
        if (origin->GetMap()->IsBattlegroundOrArena() &&
            primaryType != HsEventType::BattlegroundWon && primaryType != HsEventType::BattlegroundLost &&
            primaryType != HsEventType::ArenaWon && primaryType != HsEventType::ArenaLost)
            return;

        // The follow-up fact first, for everyone it happened to, whatever the
        // gates below decide about a reaction (see HsRecentEvent above).
        for (auto const& actor : actors)
            if (EligibleBot(actor.bot))
                NoteRecentEvent(actor.bot, actor.trigger, actor.counterpart, channel,
                                actor.affinityType == HsEventType::DuelWon || actor.affinityType == HsEventType::DuelLost);

        // A ceiling is permission, not budget. Events have no corpus
        // fallback: a canned line reacting to a specific death or roll
        // would have to be generic enough to be wrong most of the time, so
        // anything below inference is silence, not a downgrade. (A guild
        // login is the one event a canned line fits; see below.)
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
        std::vector<HsEventType>      types;
        candidates.reserve(actors.size());
        bots.reserve(actors.size());
        counterparts.reserve(actors.size());
        types.reserve(actors.size());

        for (auto const& actor : actors)
        {
            if (!EligibleBot(actor.bot))
                continue;

            uint64_t botGuid = actor.bot->GetGUID().GetRawValue();
            if (ReactedRecently(botGuid, actor.affinityType))
                continue;

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
            types.push_back(actor.affinityType);
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
            // request (and the conversation log) names who the line is aimed at
            // rather than the origin, which for a duel or a solo death is the
            // bot itself.
            Player* sender = (counterparts[index] && counterparts[index]->IsInWorld()) ? counterparts[index] : origin;

            // A guildmate coming online gets what guild chat actually says to
            // that: "o/", "hey <name>". The tuned model has no reaction rows
            // for a login and answered it as a guild join ("welcome to the
            // guild, finally", realm 2026-10-07); there is nothing a model
            // adds to a greeting that a short list does not.
            if (primaryType == HsEventType::GuildLogin)
            {
                Hs_StyleAndDeliver(bot, sender->GetGUID().GetRawValue(), sender->GetName(),
                                   GuildLoginGreeting(sender->GetName()), channel, HsChannelKind::Trade,
                                   bot->IsInCombat(), "event-canned");
                g_EventsFiredThisSession.fetch_add(1);
                continue;
            }

            HsReplyRequest request = Hs_MakeReplyRequest(bot, sender, channel, candidates[index].trigger);
            request.isEvent            = true;
            request.triggerIsStateLine = true;

            if (Hs_TryEnqueue(std::move(request)))
            {
                NoteReaction(candidates[index].botGuid, types[index]);
                g_EventsFiredThisSession.fetch_add(1);
            }
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

        // No death reactions in a battleground or arena. The killer is on the
        // other faction and cannot read it, and a match has a death every few
        // seconds: ~100 lines in one Warsong Gulch (realm 2026-10-14). The
        // match's own progress and result speak instead (OnBattlegroundUpdate,
        // OnBattlegroundEnd).
        if (player->GetMap()->IsBattlegroundOrArena())
            return;

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
                NoteRecentEvent(actor.bot, actor.trigger, nullptr, GroupChannelFor(group));
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
        // Same reason as deaths in FireDeathEvent: in a battleground or arena
        // a kill is routine, and the victim cannot read the reaction.
        if (!EligibleBot(killer) || killer->GetMap()->IsBattlegroundOrArena())
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
    // Three or more levels at once is a RandomBotLevelMgr bracket relevel,
    // not a ding (realm 2026-10-12: a level 9 bot "reached level 63" and drew
    // "U better keep up"). Same character-replaced reasoning as a drop below.
    bool const relevel = newLevel > oldlevel + 2 && Hs_IsBot(player);
    if (newLevel <= oldlevel || relevel)
    {
        // A *drop* is how mod-playerbots' recycler shows up here:
        // RandomBotLevelMgr::ResetBot knocks a bot back down a bracket in
        // place, keeping its GUID and name. Nothing about the character it
        // was survives that, so its conversation history stops being useful
        // prior-turn context and starts being wrong. This hook is the only
        // signal that covers every bot: hside_identity's own retirement
        // sweep only walks carded rows. Every map it clears is under its own
        // mutex, so this one runs inline.
        if ((newLevel < oldlevel || relevel) && Hs_IsBot(player))
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
    if (GameTime::GetUptime().count() < static_cast<int64>(g_HsGuildLoginStartupDelayMinutes) * MINUTE)
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

// ---- Battleground progress: canned lines, never the model ---------------
//
// Two kinds of callout in a battleground's raid chat, both corpus lines: the
// tuned model has no rows for mid-match play and answered every probe with
// "gg" (2026-10-14). Each needs a real player on the speaker's side; bots
// alone are talking to no one.
//
// Flags (Warsong Gulch, Eye of the Storm): a capture or a return, read off the
// scoreboard's per-player objective counters every couple of seconds, since
// the core has no hook for either. GetAttr1 is flag captures, GetAttr2 flag
// returns.
//
// Nodes (Arathi Basin, Eye of the Storm's towers, Alterac Valley, Isle of
// Conquest): what a player standing at a node can see coming, said before it
// happens, the way people call it: "inc lm 3" as enemies close on a node the
// side holds, "lm needs help" once they outnumber its defenders there, "lm
// safe" when they are gone again, "taking gm" when the side outnumbers the
// other at a node it does not hold. The speaker is a bot at or near that
// node: it is the one that sees it.
//
// All of it runs on the world thread (BattlegroundMgr::Update), so the state
// needs no lock.
namespace
{
    // Within this of a node's flag: fighting over it.
    constexpr float    kBgNodeAtRange         = 25.0f;
    // Within this: seen coming.
    constexpr float    kBgNodeIncRange        = 65.0f;
    constexpr uint32_t kBgCheckIntervalMs     = 2000;
    // At most one callout per side this often, across every node and flag.
    // Starting judgement, same status as the bias table.
    constexpr int64_t  kBgSideCooldownSeconds = 8;

    struct HsBgNode
    {
        uint8_t     index; // the battleground's own node index
        char const* name;  // what players type
        float       x, y, z;
    };

    // Positions are the core's own banner/trigger positions; names are the
    // short forms players use in /bg.
    std::vector<HsBgNode> const& NodesFor(BattlegroundTypeId type)
    {
        static std::vector<HsBgNode> const ab = [] {
            static char const* const names[] = { "st", "bs", "farm", "lm", "gm" };
            std::vector<HsBgNode> nodes;
            for (uint8_t i = 0; i < BG_AB_DYNAMIC_NODES_COUNT; ++i)
                nodes.push_back({ i, names[i], BG_AB_NodePositions[i][0], BG_AB_NodePositions[i][1], BG_AB_NodePositions[i][2] });
            return nodes;
        }();
        static std::vector<HsBgNode> const ey = [] {
            static char const* const names[] = { "fr", "be", "dr", "mt" };
            std::vector<HsBgNode> nodes;
            for (uint8_t i = 0; i < EY_POINTS_MAX; ++i)
                nodes.push_back({ i, names[i], BG_EY_TriggerPositions[i][0], BG_EY_TriggerPositions[i][1], BG_EY_TriggerPositions[i][2] });
            return nodes;
        }();
        static std::vector<HsBgNode> const av = [] {
            // BG_AV_ObjectPos's first fifteen rows are the flags, in BG_AV_Nodes order.
            static char const* const names[] = { "aid station", "sp gy", "sh gy", "sf", "ib gy", "fw gy", "hut",
                                                 "dbs", "dbn", "iwb", "shb", "ibt", "tp", "et", "wt" };
            std::vector<HsBgNode> nodes;
            for (uint8_t i = 0; i < BG_AV_NODES_MAX; ++i)
                nodes.push_back({ i, names[i], BG_AV_ObjectPos[i][0], BG_AV_ObjectPos[i][1], BG_AV_ObjectPos[i][2] });
            return nodes;
        }();
        // BattlegroundIC's nodePoint index is the ICNodePointType; the two
        // keeps are left out. Banner positions from BG_IC_ObjSpawnlocs.
        static std::vector<HsBgNode> const ic = {
            { NODE_TYPE_REFINERY, "refinery", 1269.5f,   -400.809f,  37.6253f },
            { NODE_TYPE_QUARRY,   "quarry",   251.016f,  -1159.32f,  17.2376f },
            { NODE_TYPE_DOCKS,    "docks",    726.385f,  -360.205f,  17.8153f },
            { NODE_TYPE_HANGAR,   "hangar",   807.78f,   -1000.07f,  132.381f },
            { NODE_TYPE_WORKSHOP, "ws",       776.229f,  -804.283f,  6.45052f },
        };
        static std::vector<HsBgNode> const none;

        switch (type)
        {
            case BATTLEGROUND_AB: return ab;
            case BATTLEGROUND_EY: return ey;
            case BATTLEGROUND_AV: return av;
            case BATTLEGROUND_IC: return ic;
            default:              return none;
        }
    }

    // Whose banner is on the node right now, assaulted or held. `active` false
    // for a node that no longer matters (a destroyed Alterac Valley tower).
    TeamId NodeHolder(Battleground* bg, uint8_t index, bool& active)
    {
        active = true;
        switch (bg->GetBgTypeID())
        {
            case BATTLEGROUND_AB:
            {
                uint8 state = bg->ToBattlegroundAB()->GetCapturePointInfo(index)._state;
                if (state == BG_AB_NODE_STATE_ALLY_OCCUPIED || state == BG_AB_NODE_STATE_ALLY_CONTESTED)
                    return TEAM_ALLIANCE;
                if (state == BG_AB_NODE_STATE_HORDE_OCCUPIED || state == BG_AB_NODE_STATE_HORDE_CONTESTED)
                    return TEAM_HORDE;
                return TEAM_NEUTRAL;
            }
            case BATTLEGROUND_EY:
                return bg->ToBattlegroundEY()->GetCapturePointInfo(index)._ownerTeamId;
            case BATTLEGROUND_AV:
            {
                BG_AV_NodeInfo const& info = bg->ToBattlegroundAV()->GetAVNodeInfo(index);
                active = info.State != POINT_DESTROYED;
                return info.OwnerId;
            }
            case BATTLEGROUND_IC:
            {
                ICNodeState state = bg->ToBattlegroundIC()->GetICNodePoint(index).nodeState;
                if (state == NODE_STATE_CONFLICT_A || state == NODE_STATE_CONTROLLED_A)
                    return TEAM_ALLIANCE;
                if (state == NODE_STATE_CONFLICT_H || state == NODE_STATE_CONTROLLED_H)
                    return TEAM_HORDE;
                return TEAM_NEUTRAL;
            }
            default:
                active = false;
                return TEAM_NEUTRAL;
        }
    }

    // What a side has already said about one node, so each callout is made
    // once per situation rather than every check.
    struct HsBgNodeCall
    {
        bool alerted = false; // "inc" or "needs help" said; "safe" pending
        bool helped  = false; // "needs help" said
        bool taking  = false; // "taking" said
    };

    // A Warsong Gulch flag carrier's route so far (CheckWsgRoutes).
    struct HsWsgCarrier
    {
        ObjectGuid guid;
        int        last    = -1; // last kWsgPoints index it came within range of
        int        prev    = -1; // the one before that
        int        pending = -1; // reached, not yet called
        std::chrono::steady_clock::time_point pendingAt;
    };

    struct HsBgProgress
    {
        uint32_t sinceCheckMs = 0;
        uint32_t sinceRouteMs = 0;
        HsWsgCarrier carriers[2]; // by the flag's team
        // Last-read (attr1, attr2) per player; a player's first reading only
        // sets the baseline.
        std::unordered_map<ObjectGuid::LowType, std::pair<uint32_t, uint32_t>> seen;
        std::chrono::steady_clock::time_point lastSpoke[2] = {}; // per TeamId
        std::unordered_map<uint8_t, HsBgNodeCall> calls[2];      // per TeamId, by node index
    };

    std::unordered_map<uint32_t, HsBgProgress> g_BgProgress; // by battleground instance id

    // BattlegroundScore keeps GetAttr1/GetAttr2 protected, and nothing public
    // exposes those counters. Access is not checked on the template arguments
    // of an explicit instantiation ([temp.spec.general]), so this yields
    // pointers to them; a call through one still dispatches to the
    // battleground's own score type.
    template <auto Member, typename Tag>
    struct HsScoreAttrGrant
    {
        friend auto HsScoreAttr(Tag) { return Member; }
    };
    struct HsAttr1Tag { friend auto HsScoreAttr(HsAttr1Tag); };
    struct HsAttr2Tag { friend auto HsScoreAttr(HsAttr2Tag); };
    template struct HsScoreAttrGrant<&BattlegroundScore::GetAttr1, HsAttr1Tag>;
    template struct HsScoreAttrGrant<&BattlegroundScore::GetAttr2, HsAttr2Tag>;

    bool SideHasRealPlayer(Battleground* bg, TeamId side)
    {
        for (auto const& [guid, player] : bg->GetPlayers())
            if (player && player->GetBgTeamId() == side && !Hs_IsBot(player))
                return true;
        return false;
    }

    // How often a bot makes a callout when it is the one that sees it: PvP
    // players call nearly everything, everyone else now and then. Starting
    // judgements, same status as the bias table.
    uint32_t BgCalloutPercent(uint64_t botGuid)
    {
        switch (Hs_ArchetypeForBot(botGuid))
        {
            case HsArchetype::PvpSerious: return 100;
            case HsArchetype::PvpCasual:  return 75;
            default:                      return 25;
        }
    }

    // One line from `category` (or `fixedText`, when category is null), said
    // by one of `speakers`, with %node and %n filled in. True when the moment
    // is handled: the line was said, or the chosen bot let it pass
    // (BgCalloutPercent) -- rolled once, so the check two seconds later does
    // not roll again until someone speaks. False when it could not be
    // attempted (cooldown, no speaker, no line), to retry.
    bool SpeakBgLine(HsBgProgress& state, TeamId side, std::vector<Player*> const& speakers, char const* category,
                     char const* node = "", uint32_t count = 0, char const* fixedText = nullptr)
    {
        auto const now = std::chrono::steady_clock::now();
        if (speakers.empty() || now - state.lastSpoke[side] < std::chrono::seconds(kBgSideCooldownSeconds))
            return false;

        Player*     bot  = speakers[urand(0, static_cast<uint32_t>(speakers.size() - 1))];
        if (urand(1, 100) > BgCalloutPercent(bot->GetGUID().GetRawValue()))
            return true;
        std::string line = category ? Hs_SelectOpenerLine(category, bot->getClass(), bot->GetLevel(),
                                                          static_cast<uint8_t>(bot->GetTeamId()), bot->GetZoneId())
                                    : std::string(fixedText ? fixedText : "");
        if (line.empty())
            return false;
        // %node before %n, which is its prefix.
        for (size_t at; (at = line.find("%node")) != std::string::npos;)
            line.replace(at, 5, node);
        for (size_t at; (at = line.find("%n")) != std::string::npos;)
            line.replace(at, 2, std::to_string(count));

        if (!Hs_DeliverCorpusLine(bot, line, /*senderGuid=*/0, "", GroupChannelFor(bot->GetGroup()),
                                  HsChannelKind::Trade, bot->IsInCombat(), "bg-progress"))
            return false;
        state.lastSpoke[side] = now;
        return true;
    }

    std::vector<Player*> SideSpeakers(Battleground* bg, TeamId side)
    {
        std::vector<Player*> bots;
        for (auto const& [guid, player] : bg->GetPlayers())
            if (player && player->GetBgTeamId() == side && Hs_IsBot(player) && EligibleBot(player) && player->GetGroup())
                bots.push_back(player);
        return bots;
    }

    // ---- Warsong Gulch: where a flag carrier is going ----------------------
    //
    // Points in each base (realm-surveyed with .tele add, 2026-10-15). A
    // carrier coming within kWsgPointRange of one is about to take that route.
    // In the base whose flag it holds a carrier is leaving (outbound); in its
    // own base it is coming home to cap (inbound). The carrier, if a bot, says
    // the phrase ("going tun"); a bot of the other side that can see it says
    // it about the enemy flag carrier ("efc going tun"). Held kWsgConfirmMs and
    // dropped if the carrier turned back, so a carrier that doubles back is not
    // called.
    struct HsWsgPoint
    {
        TeamId      base;   // whose base the point is in
        float       x, y, z;
        char const* out;    // leaving with this base's flag; "" = no call
        char const* in;     // coming home to cap in this base
        char const* inSelf; // the carrier's own wording for `in`, when different
    };

    HsWsgPoint const kWsgPoints[] = {
        { TEAM_HORDE,    921.60f,  1434.42f, 345.75f, "",            "on cap",       "im on cap" },
        { TEAM_HORDE,    925.43f,  1454.86f, 346.04f, "hut",         "hut",          nullptr },
        { TEAM_HORDE,    951.07f,  1422.89f, 345.37f, "",            "",             nullptr }, // main gate: wait for gy or ramp
        { TEAM_HORDE,    952.07f,  1459.45f, 342.18f, "going tun",   "tun",          nullptr },
        { TEAM_HORDE,    1114.22f, 1462.18f, 316.56f, "out tun",     "going up tun", nullptr },
        { TEAM_HORDE,    985.21f,  1458.99f, 356.22f, "going top",   "top",          nullptr },
        { TEAM_HORDE,    936.99f,  1475.66f, 367.11f, "roof",        "roof",         nullptr },
        { TEAM_HORDE,    981.66f,  1475.58f, 336.14f, "going roof",  "roof",         nullptr },
        { TEAM_HORDE,    1043.06f, 1522.87f, 337.79f, "going ramp",  "up ramp",      nullptr },
        { TEAM_HORDE,    1054.52f, 1395.30f, 339.57f, "going gy",    "gy",           nullptr },
        { TEAM_ALLIANCE, 1536.60f, 1481.47f, 352.22f, "",            "on cap",       "im on cap" },
        { TEAM_ALLIANCE, 1529.04f, 1462.13f, 352.04f, "hut",         "hut",          nullptr },
        { TEAM_ALLIANCE, 1501.81f, 1493.57f, 352.20f, "",            "",             nullptr }, // main gate: wait for gy or ramp
        { TEAM_ALLIANCE, 1492.27f, 1457.68f, 342.79f, "going tun",   "tun",          nullptr },
        { TEAM_ALLIANCE, 1370.93f, 1461.58f, 324.38f, "out tun",     "going up tun", nullptr },
        { TEAM_ALLIANCE, 1471.75f, 1458.68f, 362.56f, "going top",   "top",          nullptr },
        { TEAM_ALLIANCE, 1516.62f, 1447.95f, 373.23f, "roof",        "roof",         nullptr },
        { TEAM_ALLIANCE, 1471.11f, 1440.65f, 342.80f, "going roof",  "roof",         nullptr },
        { TEAM_ALLIANCE, 1405.73f, 1416.48f, 344.77f, "going ramp",  "up ramp",      nullptr },
        { TEAM_ALLIANCE, 1432.40f, 1528.99f, 344.63f, "going gy",    "gy",           nullptr },
    };

    // The main gate is tracked but never called: every carrier leaves through
    // it, so the route only shows at gy or ramp (operator, 2026-10-15).
    // The closest two points (cap and hut) are ~20 yd apart.
    constexpr float    kWsgPointRange   = 10.0f;
    constexpr float    kWsgSightRange   = 60.0f;
    constexpr uint32_t kWsgCheckMs      = 500;
    constexpr int64_t  kWsgConfirmMs    = 1500;

    int NearestWsgPoint(Player* carrier)
    {
        int   best     = -1;
        float bestDist = kWsgPointRange;
        for (int i = 0; i < static_cast<int>(std::size(kWsgPoints)); ++i)
        {
            float const dist = carrier->GetDistance(kWsgPoints[i].x, kWsgPoints[i].y, kWsgPoints[i].z);
            if (dist <= bestDist)
            {
                best     = i;
                bestDist = dist;
            }
        }
        return best;
    }

    void CallWsgRoute(Battleground* bg, HsBgProgress& state, Player* carrier, int point)
    {
        HsWsgPoint const& p        = kWsgPoints[point];
        TeamId const      team     = carrier->GetBgTeamId();
        bool const        inbound  = (p.base == team);
        char const*       phrase   = inbound ? p.in : p.out;
        if (!*phrase)
            return;

        // The carrier itself, to its own side.
        if (Hs_IsBot(carrier) && EligibleBot(carrier) && carrier->GetGroup() && SideHasRealPlayer(bg, team))
            SpeakBgLine(state, team, { carrier }, nullptr, "", 0, (inbound && p.inSelf) ? p.inSelf : phrase);

        // The other side, about the enemy flag carrier, from a bot that sees it.
        TeamId const other = Battleground::GetOtherTeamId(team);
        if (!SideHasRealPlayer(bg, other))
            return;
        std::vector<Player*> seeing;
        for (auto const& [guid, player] : bg->GetPlayers())
            if (player && player->GetBgTeamId() == other && player->IsAlive() && Hs_IsBot(player) &&
                EligibleBot(player) && player->GetGroup() && player->GetDistance(carrier) <= kWsgSightRange &&
                player->IsWithinLOSInMap(carrier))
                seeing.push_back(player);
        SpeakBgLine(state, other, seeing, nullptr, "", 0, ("efc " + std::string(phrase)).c_str());
    }

    void CheckWsgRoutes(Battleground* bg, HsBgProgress& state)
    {
        BattlegroundWS* ws  = bg->ToBattlegroundWS();
        auto const      now = std::chrono::steady_clock::now();
        for (TeamId flag : { TEAM_ALLIANCE, TEAM_HORDE })
        {
            HsWsgCarrier& track   = state.carriers[flag];
            ObjectGuid const guid = ws->GetFlagPickerGUID(flag);
            Player*     carrier   = guid ? ObjectAccessor::FindPlayer(guid) : nullptr;
            if (!carrier || !carrier->IsAlive() || carrier->GetMap() != bg->GetBgMap())
            {
                track = HsWsgCarrier{};
                continue;
            }
            if (track.guid != guid)
                track = HsWsgCarrier{ guid };

            int const point = NearestWsgPoint(carrier);
            if (point >= 0 && point != track.last)
            {
                track.pending   = point;
                track.pendingAt = now;
                track.prev      = track.last;
                track.last      = point;
            }

            if (track.pending < 0 ||
                std::chrono::duration_cast<std::chrono::milliseconds>(now - track.pendingAt).count() < kWsgConfirmMs)
                continue;

            // Turned back: now closer to where it came from than to where it went.
            int const  pending  = track.pending;
            track.pending       = -1;
            if (track.prev >= 0)
            {
                HsWsgPoint const& to   = kWsgPoints[pending];
                HsWsgPoint const& from = kWsgPoints[track.prev];
                if (carrier->GetDistance(from.x, from.y, from.z) < carrier->GetDistance(to.x, to.y, to.z))
                    continue;
            }
            CallWsgRoute(bg, state, carrier, pending);
        }
    }

    void CheckFlags(Battleground* bg, HsBgProgress& state)
    {
        bool took[2] = {};
        bool held[2] = {};
        auto const* scores = bg->GetPlayerScores();
        for (auto const& [guid, player] : bg->GetPlayers())
        {
            if (!player)
                continue;
            auto score = scores->find(guid.GetCounter());
            if (score == scores->end() || !score->second)
                continue;
            uint32_t const attr1 = (score->second->*HsScoreAttr(HsAttr1Tag{}))();
            uint32_t const attr2 = (score->second->*HsScoreAttr(HsAttr2Tag{}))();
            auto [entry, first] = state.seen.try_emplace(guid.GetCounter(), attr1, attr2);
            if (first)
                continue;
            TeamId const team = player->GetBgTeamId();
            if (team == TEAM_ALLIANCE || team == TEAM_HORDE)
            {
                took[team] |= attr1 > entry->second.first;
                held[team] |= attr2 > entry->second.second;
            }
            entry->second = { attr1, attr2 };
        }

        for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
        {
            TeamId const other = Battleground::GetOtherTeamId(team);
            if (took[team])
            {
                if (SideHasRealPlayer(bg, team))
                    SpeakBgLine(state, team, SideSpeakers(bg, team), "bg_flag_capped_ours");
                if (SideHasRealPlayer(bg, other))
                    SpeakBgLine(state, other, SideSpeakers(bg, other), "bg_flag_capped_theirs");
            }
            else if (held[team] && SideHasRealPlayer(bg, team))
                SpeakBgLine(state, team, SideSpeakers(bg, team), "bg_flag_returned_ours");
        }
    }

    void CheckNodes(Battleground* bg, HsBgProgress& state, std::vector<HsBgNode> const& nodes)
    {
        bool const real[2] = { SideHasRealPlayer(bg, TEAM_ALLIANCE), SideHasRealPlayer(bg, TEAM_HORDE) };
        if (!real[TEAM_ALLIANCE] && !real[TEAM_HORDE])
            return;

        for (HsBgNode const& node : nodes)
        {
            bool         active = false;
            TeamId const holder = NodeHolder(bg, node.index, active);
            if (!active)
                continue;

            // Who is where, per side: at the flag, near it, and which bots of
            // each side could see it.
            uint32_t             at[2]   = {};
            uint32_t             near[2] = {};
            std::vector<Player*> seeing[2];
            std::vector<Player*> standing[2];
            for (auto const& [guid, player] : bg->GetPlayers())
            {
                if (!player || !player->IsAlive())
                    continue;
                TeamId const team = player->GetBgTeamId();
                if (team != TEAM_ALLIANCE && team != TEAM_HORDE)
                    continue;
                float const dist = player->GetDistance(node.x, node.y, node.z);
                if (dist > kBgNodeIncRange)
                    continue;
                ++near[team];
                bool const atFlag = dist <= kBgNodeAtRange;
                at[team] += atFlag ? 1 : 0;
                if (Hs_IsBot(player) && EligibleBot(player) && player->GetGroup())
                {
                    seeing[team].push_back(player);
                    if (atFlag)
                        standing[team].push_back(player);
                }
            }

            for (TeamId side : { TEAM_ALLIANCE, TEAM_HORDE })
            {
                TeamId const  other = Battleground::GetOtherTeamId(side);
                HsBgNodeCall& call  = state.calls[side][node.index];
                if (holder == side)
                {
                    call.taking = false;
                    if (!real[side])
                        continue;
                    if (!call.helped && at[other] >= 2 && at[other] > at[side])
                    {
                        if (SpeakBgLine(state, side, seeing[side], "bg_node_help", node.name, at[other]))
                            call.alerted = call.helped = true;
                    }
                    else if (!call.alerted && near[other] > 0)
                    {
                        if (SpeakBgLine(state, side, seeing[side], "bg_node_inc", node.name, near[other]))
                            call.alerted = true;
                    }
                    else if (call.alerted && near[other] == 0)
                    {
                        // Cleared either way: a missed "safe" is not retried.
                        SpeakBgLine(state, side, seeing[side], "bg_node_safe", node.name);
                        call.alerted = call.helped = false;
                    }
                }
                else
                {
                    call.alerted = call.helped = false;
                    // A neutral node is the opening rush, not worth a call.
                    if (holder == TEAM_NEUTRAL || at[side] == 0)
                        call.taking = false;
                    else if (real[side] && !call.taking && at[side] >= 2 && at[side] > at[other] &&
                             SpeakBgLine(state, side, standing[side], "bg_node_taking", node.name, at[side]))
                        call.taking = true;
                }
            }
        }
    }
}

// ---- TEMPORARY probe (2026-10-14) ----------------------------------------
// Measures whether mod-playerbots keeps a "bg objective" set from outside:
// `.hearthside bgobj <bot> <node>` points the bot at a node, and every check
// for the next 40 seconds logs whether its objective is still that node and
// how far the bot is from it. Remove once acting on callouts is decided.
namespace
{
    struct HsBgObjProbe
    {
        ObjectGuid                            bot;
        std::string                           node;
        float                                 x = 0, y = 0, z = 0;
        std::chrono::steady_clock::time_point started, until;
    };
    std::optional<HsBgObjProbe> g_BgObjProbe;

    PositionInfo* BgObjectiveOf(Player* bot)
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai)
            return nullptr;
        return &ai->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get()["bg objective"];
    }

    void LogBgObjProbe()
    {
        if (!g_BgObjProbe)
            return;
        auto const now = std::chrono::steady_clock::now();
        Player*    bot = ObjectAccessor::FindPlayer(g_BgObjProbe->bot);
        PositionInfo* pos = bot ? BgObjectiveOf(bot) : nullptr;
        if (!pos || now > g_BgObjProbe->until)
        {
            LOG_INFO(kHsLogChat, "[HearthsideChat] bgobj probe ended.");
            g_BgObjProbe.reset();
            return;
        }
        bool const kept = pos->isSet() && std::abs(pos->x - g_BgObjProbe->x) < 1.0f && std::abs(pos->y - g_BgObjProbe->y) < 1.0f;
        LOG_INFO(kHsLogChat, "[HearthsideChat] bgobj probe +{}s {}: objective {} ({:.0f},{:.0f}), bot {:.0f} yd from {}",
                 std::chrono::duration_cast<std::chrono::seconds>(now - g_BgObjProbe->started).count(), bot->GetName(),
                 kept ? "KEPT" : (pos->isSet() ? "CHANGED" : "UNSET"), pos->x, pos->y,
                 bot->GetDistance(g_BgObjProbe->x, g_BgObjProbe->y, g_BgObjProbe->z), g_BgObjProbe->node);
    }
}

std::string Hs_ProbeBgObjective(std::string const& botName, std::string const& nodeName)
{
    Player* bot = ObjectAccessor::FindPlayerByName(botName, false);
    if (!bot || !Hs_IsBot(bot))
        return "not an online bot";
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return "not in a battleground";
    PositionInfo* pos = BgObjectiveOf(bot);
    if (!pos)
        return "no playerbot AI";
    for (HsBgNode const& node : NodesFor(bg->GetBgTypeID()))
    {
        if (nodeName != node.name)
            continue;
        pos->Set(node.x, node.y, node.z, bot->GetMapId());
        auto const now = std::chrono::steady_clock::now();
        g_BgObjProbe = HsBgObjProbe{ bot->GetGUID(), node.name, node.x, node.y, node.z, now, now + std::chrono::seconds(40) };
        return "objective set to " + nodeName + "; watch Server.log for 'bgobj probe'";
    }
    return "no node '" + nodeName + "' in this battleground";
}

void HsEventBattlegroundHandler::OnBattlegroundUpdate(Battleground* bg, uint32 diff)
{
    if (!g_HsEnable || !bg || bg->isArena() || bg->GetStatus() != STATUS_IN_PROGRESS ||
        !HsTierAllows(g_HsMaxTierEvents, HsTier::Corpus))
        return;
    BattlegroundTypeId const       type  = bg->GetBgTypeID();
    std::vector<HsBgNode> const&   nodes = NodesFor(type);
    bool const                     flags = (type == BATTLEGROUND_WS || type == BATTLEGROUND_EY);
    if (!flags && nodes.empty())
        return;

    HsBgProgress& state = g_BgProgress[bg->GetInstanceID()];

    // Carrier routes on a shorter clock: a gate is crossed in a second or two.
    if (type == BATTLEGROUND_WS)
    {
        state.sinceRouteMs += diff;
        if (state.sinceRouteMs >= kWsgCheckMs)
        {
            state.sinceRouteMs = 0;
            CheckWsgRoutes(bg, state);
        }
    }

    state.sinceCheckMs += diff;
    if (state.sinceCheckMs < kBgCheckIntervalMs)
        return;
    state.sinceCheckMs = 0;

    LogBgObjProbe();
    if (flags)
        CheckFlags(bg, state);
    if (!nodes.empty())
        CheckNodes(bg, state, nodes);
}

void HsEventBattlegroundHandler::OnBattlegroundDestroy(Battleground* bg)
{
    if (bg)
        g_BgProgress.erase(bg->GetInstanceID());
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

std::string Hs_RecentEventContext(uint64_t botGuid, uint64_t senderGuid, HsReplyChannel replyChannel)
{
    std::lock_guard<std::mutex> lock(g_RecentEventMutex);
    auto it = g_RecentEvents.find(botGuid);
    if (it == g_RecentEvents.end())
        return "";

    HsRecentEvent const& recent = it->second;
    auto age = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - recent.at).count();
    if (age > kRecentEventWindowSeconds)
        return "";

    // Party and raid chat reach the same group, whichever one the event used.
    auto audience = [](HsReplyChannel c) { return c == HsReplyChannel::Raid ? HsReplyChannel::Party : c; };
    bool const counterpart = senderGuid && senderGuid == recent.counterpartGuid;
    if (!counterpart && audience(replyChannel) != audience(recent.channel))
        return "";

    // The model is never told who is speaking to it, so a winner's "loser"
    // and a bystander's read the same. When they are the other person in the
    // event, it says so.
    std::string out = recent.line;
    if (counterpart && !recent.counterpartName.empty())
        out += "\nThe one talking to you now is " + recent.counterpartName + ".";
    return out;
}

bool Hs_HasRecentEvent(uint64_t botGuid)
{
    std::lock_guard<std::mutex> lock(g_RecentEventMutex);
    auto it = g_RecentEvents.find(botGuid);
    return it != g_RecentEvents.end() &&
           std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - it->second.at).count()
               <= kRecentEventWindowSeconds;
}

// One comeback per duel, a third of the time: "gg, u got lucky" -> "wasn't
// luck". Not every taunt gets one, and the comeback never gets one back. A
// starting judgement, same status as the bias table.
constexpr uint32_t kDuelComebackPercent = 35;

void Hs_MaybeAnswerDuelLine(Player* speaker, uint64_t opponentGuid, std::string const& text)
{
    if (!g_HsEnable || !speaker || !opponentGuid || text.empty() || !HsTierAllows(g_HsMaxTierEvents, HsTier::Inference))
        return;

    uint64_t const speakerGuid = speaker->GetGUID().GetRawValue();
    {
        // Both sides must still hold this duel as their latest event, and
        // claiming it spends it for both, so the comeback (delivered as an
        // event line itself) can never draw another.
        std::lock_guard<std::mutex> lock(g_RecentEventMutex);
        auto mine   = g_RecentEvents.find(speakerGuid);
        auto theirs = g_RecentEvents.find(opponentGuid);
        if (mine == g_RecentEvents.end() || theirs == g_RecentEvents.end())
            return;
        HsRecentEvent& a = mine->second;
        HsRecentEvent& b = theirs->second;
        if (!a.duelEnd || !b.duelEnd || a.answered || b.answered ||
            a.counterpartGuid != opponentGuid || b.counterpartGuid != speakerGuid)
            return;
        auto const now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - b.at).count() > kRecentEventWindowSeconds)
            return;
        a.answered = b.answered = true;
    }
    if (urand(1, 100) > kDuelComebackPercent)
        return;

    Player* opponent = ObjectAccessor::FindPlayer(ObjectGuid(opponentGuid));
    if (!EligibleBot(opponent) || !opponent->IsAlive() || !opponent->IsWithinDistInMap(speaker, g_HsSayDistance))
        return;

    // Answered like any line said to it after the duel: the worker appends
    // the duel state line and "The one talking to you now is X." -- the
    // post-event follow-up shape the tune already trains on.
    HsReplyRequest request = Hs_MakeReplyRequest(opponent, speaker, HsReplyChannel::Say, text);
    request.isEvent = true;
    Hs_TryEnqueue(std::move(request));
}

uint32_t Hs_EventsFiredThisSession()
{
    return g_EventsFiredThisSession.load();
}
