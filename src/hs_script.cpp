#include "hs_script.h"
#include "hs_archetype.h"
#include "hs_channel.h"
#include "hs_config.h"
#include "hs_corpus.h"
#include "hs_prune.h"
#include "hs_queue.h" // §4.17: Hs_ResolveChannelForDelivery, HsReplyChannel::Channel's delivery pattern
#include "hs_rpgstate.h"
#include "hs_style.h"
#include "hs_tier.h"

#include "Channel.h"    // §4.17 channel scripts: Channel::Say
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Random.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    // Script fire rate and reserve behaviour are live-realm-only
    // judgements, so these are placeholder constants, not config keys.
    //
    // The proximity-scene fire chance is the exception: it moved to
    // Script.Proximity.FireChancePercent (hs_config.h) when the
    // settled-state gate landed, since how much that gate cuts depends on
    // what the realm's bots are doing at the time and cannot be guessed
    // here. kScanIntervalMs x that key is still the burn-rate knob it
    // always was.
    constexpr uint32_t kScanIntervalMs          = 30000; // how often we even look
    constexpr uint32_t kWitnessCooldownSeconds  = 300;   // don't re-fire near the same player too soon
    constexpr uint32_t kFirstTurnDelayMinMs     = 800;
    constexpr uint32_t kFirstTurnDelayMaxMs     = 2000;
    constexpr uint32_t kTurnGapMinSeconds       = 4;
    constexpr uint32_t kTurnGapMaxSeconds       = 7;

    bool IsBot(Player* p)
    {
        if (!p)
            return false;
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(p);
        return ai && ai->IsBotAI();
    }

    // HearthsideChat.ExcludeNames -- "no reflex, grounded, corpus, or
    // reactive reply, ever" (hs_config.h). A scripted turn is corpus content
    // the bot speaks unprompted, so an excluded bot may not be cast as
    // either speaker in a /say scene or a channel scene. Both scans below
    // use this in place of a bare IsBot: unlike hs_opener.cpp, neither scan
    // needs to tell bots from real players, so there is no second predicate.
    bool IsEligibleBot(Player* p)
    {
        return IsBot(p) && !Hs_IsExcludedBotName(p->GetName());
    }

    struct HsActiveScriptRun
    {
        uint64_t bot0Guid;
        uint64_t bot1Guid;
        uint64_t witnessGuid;
        bool     aborted;
        uint32_t turnsRemaining;
    };

    struct HsScheduledTurn
    {
        uint32_t          runId;
        uint64_t          speakerGuid;
        std::string       text;
        Clock::time_point deliverAt;
    };

    std::mutex                                    g_RunsMutex;
    std::unordered_map<uint32_t, HsActiveScriptRun> g_ActiveRuns;
    uint32_t                                       g_NextRunId = 1;

    std::mutex                     g_ScheduleMutex;
    std::deque<HsScheduledTurn>    g_ScheduledTurns;

    std::mutex                                      g_WitnessCooldownMutex;
    std::unordered_map<uint64_t, Clock::time_point>  g_LastWitnessAt;

    uint32_t g_ScanAccumulatorMs = 0;

    // ---- §4.17: channel scripts. No witness/abort concept -- a channel
    // cast needn't be co-located and there's no single player to interrupt
    // on (the whole channel is the audience), so this state is deliberately
    // smaller than HsActiveScriptRun/HsScheduledTurn above: no aborted flag,
    // no witnessGuid. 2 turns, not 4 (kChannelScriptTurnDelayMinMs/MaxMs
    // below reuse the same first-turn/turn-gap shape as the /say path).
    struct HsActiveChannelRun
    {
        uint64_t      bot0Guid;
        uint64_t      bot1Guid;
        uint32_t      turnsRemaining;
    };

    struct HsScheduledChannelTurn
    {
        uint32_t          runId;
        uint64_t          speakerGuid;
        uint64_t          listenerGuid; // %other_* placeholder resolution
        HsChannelKind     kind;
        std::string       text;
        Clock::time_point deliverAt;
    };

    std::mutex                                       g_ChannelRunsMutex;
    std::unordered_map<uint32_t, HsActiveChannelRun> g_ActiveChannelRuns;
    uint32_t                                          g_NextChannelRunId = 1;

    std::mutex                          g_ChannelScheduleMutex;
    std::deque<HsScheduledChannelTurn>  g_ScheduledChannelTurns;

    uint32_t g_ChannelScanAccumulatorMs = 0;

    // A placeholder starting guess, same footing kScanIntervalMs
    // above is on -- channel scripts additionally gate on each channel's own
    // MaxTier (checked per kind at fire time, not here).
    constexpr uint32_t kChannelScanFireChancePercent = 3;

    bool IsBotInActiveChannelRun(uint64_t botGuid)
    {
        std::lock_guard<std::mutex> lock(g_ChannelRunsMutex);
        for (auto const& entry : g_ActiveChannelRuns)
            if (entry.second.bot0Guid == botGuid || entry.second.bot1Guid == botGuid)
                return true;
        return false;
    }

    bool WitnessCooldownOk(uint64_t playerGuid)
    {
        std::lock_guard<std::mutex> lock(g_WitnessCooldownMutex);
        auto it = g_LastWitnessAt.find(playerGuid);
        if (it == g_LastWitnessAt.end())
            return true;
        auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - it->second).count();
        return elapsedSec >= static_cast<int64_t>(kWitnessCooldownSeconds);
    }

    void MarkWitnessCooldown(uint64_t playerGuid)
    {
        std::lock_guard<std::mutex> lock(g_WitnessCooldownMutex);
        Clock::time_point now = Clock::now();
        g_LastWitnessAt[playerGuid] = now;
        // Read only through kWitnessCooldownSeconds, so anything well past
        // that window answers as a missing key would (hs_prune.h).
        HsPrune::PruneStale(g_LastWitnessAt, now,
                            /*staleSeconds=*/static_cast<int64_t>(kWitnessCooldownSeconds) * 4,
                            /*pruneAboveSize=*/512);
    }

    bool IsBotInActiveRun(uint64_t botGuid)
    {
        std::lock_guard<std::mutex> lock(g_RunsMutex);
        for (auto const& entry : g_ActiveRuns)
            if (!entry.second.aborted && (entry.second.bot0Guid == botGuid || entry.second.bot1Guid == botGuid))
                return true;
        return false;
    }

    // Claims one available script (single consumer -- only this scan ever
    // writes hside_script.consumed_at -- so a plain SELECT-then-UPDATE has
    // no concurrent claimant to race against) and schedules its turns,
    // staggered by a per-turn typing delay so they don't land in a burst.
    void ClaimAndSchedule(Player* bot0, Player* bot1, Player* witness)
    {
        QueryResult idResult = CharacterDatabase.Query(
            "SELECT id FROM hside_script WHERE consumed_at IS NULL AND channel IS NULL ORDER BY id LIMIT 1");
        if (!idResult)
            return; // reserve dry -- running dry is the correct failure mode, not an error
        uint32_t scriptId = (*idResult)[0].Get<uint32_t>();

        // PLAN-AMBIENT.md §2's shared unprompted-speech budget (hs_queue.h).
        // Spent once per *scene*, not per turn: a four-turn exchange is one
        // thing a listener perceives, and charging four tokens for it would
        // let a single scene drain a budget sized for the whole realm.
        //
        // Placed between the availability check and the claiming UPDATE
        // specifically. Earlier would burn a token whenever the reserve is
        // dry; later would consume a script the budget then refuses to let
        // anyone hear. Here, a denied budget costs one wasted SELECT and
        // leaves the script unclaimed for the next scan.
        if (!Hs_AmbientBucketTake())
            return;

        QueryResult turnResult = CharacterDatabase.Query(
            "SELECT speaker_slot, text FROM hside_script_turn WHERE script_id = {} ORDER BY turn_no", scriptId);
        if (!turnResult)
            return; // defensive: a header row with no turns should never exist

        std::vector<std::pair<uint8_t, std::string>> turns;
        do
        {
            turns.emplace_back((*turnResult)[0].Get<uint8_t>(), (*turnResult)[1].Get<std::string>());
        } while (turnResult->NextRow());

        uint64_t bot0Guid    = bot0->GetGUID().GetRawValue();
        uint64_t bot1Guid    = bot1->GetGUID().GetRawValue();
        uint64_t witnessGuid = witness->GetGUID().GetRawValue();

        CharacterDatabase.Execute(
            "UPDATE hside_script SET consumed_at = NOW(), consumed_by_zone = {}, consumed_witness = {} WHERE id = {}",
            witness->GetZoneId(), witnessGuid, scriptId);

        uint32_t runId;
        {
            std::lock_guard<std::mutex> lock(g_RunsMutex);
            runId = g_NextRunId++;
            g_ActiveRuns[runId] = HsActiveScriptRun{ bot0Guid, bot1Guid, witnessGuid, false,
                                                       static_cast<uint32_t>(turns.size()) };
        }

        Clock::time_point deliverAt = Clock::now() + std::chrono::milliseconds(urand(kFirstTurnDelayMinMs, kFirstTurnDelayMaxMs));
        {
            std::lock_guard<std::mutex> lock(g_ScheduleMutex);
            for (auto const& turn : turns)
            {
                uint64_t speakerGuid = turn.first == 0 ? bot0Guid : bot1Guid;
                g_ScheduledTurns.push_back({ runId, speakerGuid, turn.second, deliverAt });
                deliverAt += std::chrono::seconds(urand(kTurnGapMinSeconds, kTurnGapMaxSeconds));
            }
        }

        MarkWitnessCooldown(witnessGuid);
    }

    void TryFireNearPlayer(Player* player)
    {
        uint64_t playerGuid = player->GetGUID().GetRawValue();
        if (!WitnessCooldownOk(playerGuid))
            return;

        std::vector<Player*> nearbyBots;
        for (auto const& itr : ObjectAccessor::GetPlayers())
        {
            Player* candidate = itr.second;
            if (!candidate || candidate == player || !candidate->IsInWorld())
                continue;
            if (!IsEligibleBot(candidate)) // incl. HearthsideChat.ExcludeNames
                continue;
            if (candidate->GetTeamId() != player->GetTeamId())
                continue;
            if (candidate->IsInCombat() || !candidate->IsAlive())
                continue;
            // Both participants must be settled -- stationary and resting
            // or loitering (Hs_IsBotSettled, hs_rpgstate.h). Unlike
            // ambient's companion,
            // neither of these two is scenery: they are about to hold a
            // multi-turn conversation in /say over the next several minutes,
            // and a bot that walks off to a quest objective mid-scene leaves
            // the other one talking to nobody. DeliverOneTurn's per-turn
            // re-check catches that as a range abort, but not starting a
            // scene with a bot that was already on its way somewhere is the
            // better fix.
            if (!Hs_IsBotSettled(candidate))
                continue;
            // Map- and phase-aware; see hs_handler.cpp's /say eligibility
            // filter for why the bare GetDistance is wrong here.
            if (!candidate->IsWithinDistInMap(player, g_HsSayDistance))
                continue;
            if (IsBotInActiveRun(candidate->GetGUID().GetRawValue()))
                continue;
            nearbyBots.push_back(candidate);
            if (nearbyBots.size() >= 2)
                break;
        }

        if (nearbyBots.size() < 2)
            return;
        if (urand(0, 99) >= g_HsScriptProximityFireChancePercent)
            return;

        ClaimAndSchedule(nearbyBots[0], nearbyBots[1], player);
    }

    // Re-checks every abort condition immediately before sending -- schedule
    // time and delivery time can be minutes apart for a script's later
    // turns, and a participant leaving range, entering combat, or dying
    // needs to be caught whenever it actually happens, not just at the
    // start.
    void DeliverOneTurn(const HsScheduledTurn& scheduled)
    {
        bool     aborted = false;
        uint64_t bot0Guid = 0, bot1Guid = 0, witnessGuid = 0;

        {
            std::lock_guard<std::mutex> lock(g_RunsMutex);
            auto it = g_ActiveRuns.find(scheduled.runId);
            if (it == g_ActiveRuns.end())
                return; // defensive -- shouldn't happen
            HsActiveScriptRun& run = it->second;
            aborted     = run.aborted;
            bot0Guid    = run.bot0Guid;
            bot1Guid    = run.bot1Guid;
            witnessGuid = run.witnessGuid;

            if (--run.turnsRemaining == 0)
                g_ActiveRuns.erase(it); // last turn of this run -- free the participants either way
        }

        if (aborted)
            return;

        Player* bot0    = ObjectAccessor::FindPlayer(ObjectGuid(bot0Guid));
        Player* bot1    = ObjectAccessor::FindPlayer(ObjectGuid(bot1Guid));
        Player* witness = ObjectAccessor::FindPlayer(ObjectGuid(witnessGuid));

        bool ok = bot0 && bot0->IsInWorld() && bot0->IsAlive() && !bot0->IsInCombat()
               && bot1 && bot1->IsInWorld() && bot1->IsAlive() && !bot1->IsInCombat()
               && witness && witness->IsInWorld() && witness->IsAlive()
               && bot0->IsWithinDistInMap(witness, g_HsSayDistance)
               && bot1->IsWithinDistInMap(witness, g_HsSayDistance);

        if (!ok)
        {
            // Mark aborted so any later turns of this run still pending
            // (the run may not have been erased above if this wasn't the
            // last one) skip too, rather than resurrecting a broken scene.
            std::lock_guard<std::mutex> lock(g_RunsMutex);
            auto it = g_ActiveRuns.find(scheduled.runId);
            if (it != g_ActiveRuns.end())
                it->second.aborted = true;
            return;
        }

        Player* speaker = (scheduled.speakerGuid == bot0Guid) ? bot0 : bot1;
        Player* listener = (speaker == bot0) ? bot1 : bot0;
        PlayerbotAI* speakerAI = PlayerbotsMgr::instance().GetPlayerbotAI(speaker);
        if (!speakerAI)
            return;

        // %my_*/%other_* resolution (§4.16), before the style pass so a
        // typo/abbreviation transform never touches a still-live token.
        // Skips just this turn, not the whole run, on an unresolvable field
        // (e.g. the listener is unguilded) -- the same "no substitute"
        // shape used elsewhere in the module.
        std::string text = scheduled.text;
        if (text.find('%') != std::string::npos)
        {
            if (!Hs_ResolveScriptPlaceholders(text, Hs_BuildPlaceholderContext(speaker), Hs_BuildPlaceholderContext(listener)))
                return;
        }

        // No archetype/persona goes into script generation, but the style
        // pass still runs per speaker at delivery -- the same script
        // spoken by two different bots reads as two different people.
        HsArchetype             archetype     = Hs_ArchetypeForBot(scheduled.speakerGuid, speaker->GetLevel());
        HsArchetypeInfo const   archetypeInfo = Hs_ArchetypeInfoFor(archetype);
        HsStyleContext styleCtx;
        styleCtx.baselineCare         = archetypeInfo.care;
        styleCtx.abbrevOverrideChance = archetypeInfo.hasAbbrevOverride ? archetypeInfo.abbrevOverrideChance : -1.0f;
        styleCtx.inCombat             = false; // already confirmed not in combat above
        HsStyleResult style = Hs_ApplyStyle(scheduled.speakerGuid, speaker->GetName(), witness->GetName(), text, styleCtx);
        if (style.text.empty())
            return;

        speakerAI->Say(style.text);
    }

    void DeliverPendingTurns()
    {
        std::deque<HsScheduledTurn> ready;
        {
            std::lock_guard<std::mutex> lock(g_ScheduleMutex);
            if (g_ScheduledTurns.empty())
                return;
            Clock::time_point now = Clock::now();
            auto notYetReady = std::stable_partition(g_ScheduledTurns.begin(), g_ScheduledTurns.end(),
                [now](const HsScheduledTurn& t) { return t.deliverAt <= now; });
            ready.assign(g_ScheduledTurns.begin(), notYetReady);
            g_ScheduledTurns.erase(g_ScheduledTurns.begin(), notYetReady);
            if (ready.empty())
                return;
        }

        for (auto const& turn : ready)
            DeliverOneTurn(turn);
    }

    // §4.17: claims one 2-turn channel script (hside_script.channel = the
    // kind's lowercase name) and schedules its turns -- same shape as
    // ClaimAndSchedule above, minus the witness bookkeeping that mechanism
    // has no equivalent for.
    void ClaimAndScheduleChannel(HsChannelKind kind, Player* bot0, Player* bot1)
    {
        std::string channelColumn = std::string(Hs_ChannelKindName(kind));
        std::transform(channelColumn.begin(), channelColumn.end(), channelColumn.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        QueryResult idResult = CharacterDatabase.Query(
            "SELECT id FROM hside_script WHERE consumed_at IS NULL AND channel = '{}' ORDER BY id LIMIT 1",
            channelColumn);
        if (!idResult)
            return; // reserve dry
        uint32_t scriptId = (*idResult)[0].Get<uint32_t>();

        // Shared unprompted-speech budget, same placement and reasoning as
        // ClaimAndSchedule above. A channel scene is charged the same single
        // token a /say scene is -- it reaches a wider audience, but it is
        // still one exchange, and this budget measures how often bots talk
        // among themselves rather than how many players overhear it.
        if (!Hs_AmbientBucketTake())
            return;

        QueryResult turnResult = CharacterDatabase.Query(
            "SELECT speaker_slot, text FROM hside_script_turn WHERE script_id = {} ORDER BY turn_no", scriptId);
        if (!turnResult)
            return; // defensive: a header row with no turns should never exist

        std::vector<std::pair<uint8_t, std::string>> turns;
        do
        {
            turns.emplace_back((*turnResult)[0].Get<uint8_t>(), (*turnResult)[1].Get<std::string>());
        } while (turnResult->NextRow());

        uint64_t bot0Guid = bot0->GetGUID().GetRawValue();
        uint64_t bot1Guid = bot1->GetGUID().GetRawValue();

        CharacterDatabase.Execute(
            "UPDATE hside_script SET consumed_at = NOW(), consumed_by_zone = {}, consumed_witness = NULL WHERE id = {}",
            bot0->GetZoneId(), scriptId);

        uint32_t runId;
        {
            std::lock_guard<std::mutex> lock(g_ChannelRunsMutex);
            runId = g_NextChannelRunId++;
            g_ActiveChannelRuns[runId] = HsActiveChannelRun{ bot0Guid, bot1Guid, static_cast<uint32_t>(turns.size()) };
        }

        Clock::time_point deliverAt = Clock::now() + std::chrono::milliseconds(urand(kFirstTurnDelayMinMs, kFirstTurnDelayMaxMs));
        {
            std::lock_guard<std::mutex> lock(g_ChannelScheduleMutex);
            for (auto const& turn : turns)
            {
                uint64_t speakerGuid  = turn.first == 0 ? bot0Guid : bot1Guid;
                uint64_t listenerGuid = turn.first == 0 ? bot1Guid : bot0Guid;
                g_ScheduledChannelTurns.push_back({ runId, speakerGuid, listenerGuid, kind, turn.second, deliverAt });
                deliverAt += std::chrono::seconds(urand(kTurnGapMinSeconds, kTurnGapMaxSeconds));
            }
        }
    }

    // Finds two same-team bots that both resolve to the same live channel
    // instance for `kind` (Hs_ResolveChannelForDelivery, hs_queue.h -- the
    // same zone-qualified resolution delivery uses, so "grouped by resolved
    // Channel*" is equivalent to "members of the same channel instance"),
    // confirmed via the public Player::IsInChannel rather than trusted on
    // Hs_ResolveChannelForDelivery's return alone. No proximity or combat
    // check (§4.17's channel cast needn't be co-located) -- only alive,
    // same team, and not already mid-script (either mechanism).
    //
    // The instance also has to have a real player in it. Unlike the /say
    // scene scan, which gets its audience from the witness search, a channel
    // scene has no such constraint on the cast, so the audience test has to
    // be made explicitly against the instance itself.
    void TryFireChannelScript(HsChannelKind kind)
    {
        if (!HsTierAllows(Hs_ChannelPolicyFor(kind).maxTier, HsTier::Corpus))
            return; // this channel's own MaxTier, independent of MaxTier.BotToBot

        std::unordered_map<Channel*, std::vector<Player*>> byInstance;
        std::vector<Player*>                                realPlayers;
        for (auto const& itr : ObjectAccessor::GetPlayers())
        {
            Player* candidate = itr.second;
            if (!candidate || !candidate->IsInWorld())
                continue;

            // Real players are collected unresolved, for the reason
            // hs_ambient.cpp's own channel scan spells out: resolving a human
            // through Hs_ResolveChannelForDelivery would make this scan depend
            // on that function agreeing with the core about every channel's
            // name, and a disagreement silences the surface instead of failing
            // visibly. Membership is tested below against the bot-resolved
            // Channel*, which is exact because it is the same object.
            //
            // Note this is a bare IsBot, not IsEligibleBot: an excluded bot
            // is not a valid speaker, but it is also not the human whose
            // presence makes a scene worth performing.
            if (!IsBot(candidate))
            {
                realPlayers.push_back(candidate);
                continue;
            }

            // IsEligibleBot covers HearthsideChat.ExcludeNames, and is
            // deliberately ahead of the Hs_ResolveChannelForDelivery call
            // below -- that one is a DBC lookup plus a ChannelMgr string
            // match, by far the most expensive test in this loop.
            if (!IsEligibleBot(candidate) || !candidate->IsAlive())
                continue;
            uint64_t guid = candidate->GetGUID().GetRawValue();
            if (IsBotInActiveRun(guid) || IsBotInActiveChannelRun(guid))
                continue;

            Channel* channel = Hs_ResolveChannelForDelivery(candidate, kind);
            if (!channel || !candidate->IsInChannel(channel))
                continue;
            byInstance[channel].push_back(candidate);
        }

        // Collect every instance that can carry a scene, then pick among
        // them, rather than taking the first one enumerated. Two changes in
        // one, both copied from hs_ambient.cpp's channel scan:
        //
        //   - A real player has to be in the instance. Global channels are
        //     one Channel object per zone, so a realm with bots spread over
        //     forty zones has forty General instances and a player standing
        //     in one of them. Without this test a scene fires into whichever
        //     instance enumerated first, is logged as delivered, is written
        //     to hside_chat_log, and is heard by nobody -- measured at ~98%
        //     inaudible on the test realm (334 bots, 8 of them sharing the
        //     player's instance).
        //   - Picking uniformly rather than taking the first, so that on a
        //     realm with several populated cities one instance cannot
        //     monopolize the surface.
        std::vector<std::vector<Player*>*> eligibleInstances;
        for (auto& entry : byInstance)
        {
            if (entry.second.size() < 2)
                continue;

            bool heard = false;
            for (Player* player : realPlayers)
            {
                if (player->IsInChannel(entry.first))
                {
                    heard = true;
                    break; // presence test, not a count
                }
            }
            if (!heard)
                continue;

            eligibleInstances.push_back(&entry.second);
        }
        if (eligibleInstances.empty())
            return;

        std::vector<Player*>* pool =
            eligibleInstances[urand(0, static_cast<uint32_t>(eligibleInstances.size() - 1))];
        if (urand(0, 99) >= kChannelScanFireChancePercent)
            return;

        // Two distinct random members of the same pool.
        Player* bot0 = (*pool)[urand(0, static_cast<uint32_t>(pool->size() - 1))];
        Player* bot1 = bot0;
        for (int attempt = 0; attempt < 5 && bot1 == bot0; ++attempt)
            bot1 = (*pool)[urand(0, static_cast<uint32_t>(pool->size() - 1))];
        if (bot1 == bot0)
            return; // defensive -- shouldn't happen with size() >= 2

        ClaimAndScheduleChannel(kind, bot0, bot1);
    }

    void DeliverOneChannelTurn(const HsScheduledChannelTurn& scheduled)
    {
        {
            // Decrements turnsRemaining and frees the run slot on its last
            // turn either way -- run bookkeeping only, the speaker/listener
            // guids scheduled per-turn are what delivery actually uses.
            std::lock_guard<std::mutex> lock(g_ChannelRunsMutex);
            auto it = g_ActiveChannelRuns.find(scheduled.runId);
            if (it == g_ActiveChannelRuns.end())
                return; // defensive -- shouldn't happen
            if (--it->second.turnsRemaining == 0)
                g_ActiveChannelRuns.erase(it);
        }

        Player* speaker  = ObjectAccessor::FindPlayer(ObjectGuid(scheduled.speakerGuid));
        Player* listener = ObjectAccessor::FindPlayer(ObjectGuid(scheduled.listenerGuid));
        if (!speaker || !speaker->IsInWorld() || !speaker->IsAlive())
            return; // no proximity/combat re-check by design (§4.17) -- alive+online is the floor

        PlayerbotAI* speakerAI = PlayerbotsMgr::instance().GetPlayerbotAI(speaker);
        if (!speakerAI)
            return;

        std::string text = scheduled.text;
        if (text.find('%') != std::string::npos)
        {
            // listener may have logged off mid-run -- an unresolvable
            // %other_* field drops just this turn, same "no substitute"
            // contract as the /say path.
            HsPlaceholderContext otherCtx = listener ? Hs_BuildPlaceholderContext(listener) : HsPlaceholderContext{};
            if (!Hs_ResolveScriptPlaceholders(text, Hs_BuildPlaceholderContext(speaker), otherCtx))
                return;
        }

        HsArchetype             archetype     = Hs_ArchetypeForBot(scheduled.speakerGuid, speaker->GetLevel());
        HsArchetypeInfo const   archetypeInfo = Hs_ArchetypeInfoFor(archetype);
        HsStyleContext styleCtx;
        styleCtx.baselineCare         = archetypeInfo.care;
        styleCtx.abbrevOverrideChance = archetypeInfo.hasAbbrevOverride ? archetypeInfo.abbrevOverrideChance : -1.0f;
        styleCtx.inCombat             = speaker->IsInCombat();
        styleCtx.tradeCareOffset      = Hs_TradeCareOffsetFor(scheduled.speakerGuid);
        HsStyleResult style = Hs_ApplyStyle(scheduled.speakerGuid, speaker->GetName(), "", text, styleCtx);
        if (style.text.empty())
            return;

        Channel* channel = Hs_ResolveChannelForDelivery(speaker, scheduled.kind);
        if (!channel)
            return; // speaker no longer resolves to that channel instance (e.g. moved zones) -- drop, don't misdeliver
        channel->Say(speaker->GetGUID(), style.text, LANG_UNIVERSAL);
    }

    void DeliverPendingChannelTurns()
    {
        std::deque<HsScheduledChannelTurn> ready;
        {
            std::lock_guard<std::mutex> lock(g_ChannelScheduleMutex);
            if (g_ScheduledChannelTurns.empty())
                return;
            Clock::time_point now = Clock::now();
            auto notYetReady = std::stable_partition(g_ScheduledChannelTurns.begin(), g_ScheduledChannelTurns.end(),
                [now](const HsScheduledChannelTurn& t) { return t.deliverAt <= now; });
            ready.assign(g_ScheduledChannelTurns.begin(), notYetReady);
            g_ScheduledChannelTurns.erase(g_ScheduledChannelTurns.begin(), notYetReady);
            if (ready.empty())
                return;
        }

        for (auto const& turn : ready)
            DeliverOneChannelTurn(turn);
    }
}

void HsScriptRunnerWorldScript::OnUpdate(uint32_t diff)
{
    // Fine-grained every tick -- turn pacing depends on it. Delivery always
    // drains regardless of the tier gates below, same as the /say path.
    DeliverPendingTurns();
    DeliverPendingChannelTurns();

    if (!g_HsEnable)
        return;

    if (HsTierAllows(HsParseTier(g_HsMaxTierBotToBot), HsTier::Corpus)) // corpus-only in v1
    {
        g_ScanAccumulatorMs += diff;
        if (g_ScanAccumulatorMs >= kScanIntervalMs)
        {
            g_ScanAccumulatorMs = 0;
            for (auto const& itr : ObjectAccessor::GetPlayers())
            {
                Player* player = itr.second;
                if (!player || !player->IsInWorld() || IsBot(player))
                    continue;
                TryFireNearPlayer(player);
            }
        }
    }

    // §4.17: independent of MaxTier.BotToBot -- each channel's own MaxTier
    // gates it (checked inside TryFireChannelScript). Shares this
    // WorldScript's tick and scan cadence rather than running a second
    // near-identical timer, same reasoning §4.22 gives for sharing a scan
    // thread with the opener's fifth trigger.
    g_ChannelScanAccumulatorMs += diff;
    if (g_ChannelScanAccumulatorMs >= kScanIntervalMs)
    {
        g_ChannelScanAccumulatorMs = 0;
        TryFireChannelScript(HsChannelKind::Trade);
        TryFireChannelScript(HsChannelKind::General);
    }
}

void Hs_AbortScriptsWitnessedBy(uint64_t playerGuid)
{
    std::lock_guard<std::mutex> lock(g_RunsMutex);
    for (auto& entry : g_ActiveRuns)
        if (entry.second.witnessGuid == playerGuid)
            entry.second.aborted = true;
}

bool Hs_IsBotInAnyScriptRun(uint64_t botGuid)
{
    // Both file-local predicates, exposed as one call rather than two so a
    // caller cannot check only half of "is this bot busy" -- the /say and
    // channel mechanisms are separate bookkeeping but the same speaker.
    return IsBotInActiveRun(botGuid) || IsBotInActiveChannelRun(botGuid);
}

uint32_t Hs_ActiveScriptRunCount()
{
    std::lock_guard<std::mutex> lock(g_RunsMutex);
    return static_cast<uint32_t>(g_ActiveRuns.size());
}

uint32_t Hs_ScriptsConsumedLast24h()
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT COUNT(*) FROM hside_script WHERE consumed_at IS NOT NULL AND consumed_at >= NOW() - INTERVAL 1 DAY");
    return result ? (*result)[0].Get<uint32_t>() : 0;
}
