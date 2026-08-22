#include "hs_script.h"
#include "hs_archetype.h"
#include "hs_config.h"
#include "hs_style.h"
#include "hs_tier.h"

#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Random.h"

#include <algorithm>
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
    // kScanIntervalMs x kScanFireChancePercent targets roughly a 10-minute
    // burn rate, without claiming that figure is measured.
    constexpr uint32_t kScanIntervalMs          = 30000; // how often we even look
    constexpr uint32_t kScanFireChancePercent   = 5;     // per eligible duo, per scan
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
        g_LastWitnessAt[playerGuid] = Clock::now();
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
            "SELECT id FROM hside_script WHERE consumed_at IS NULL ORDER BY id LIMIT 1");
        if (!idResult)
            return; // reserve dry -- running dry is the correct failure mode, not an error
        uint32_t scriptId = (*idResult)[0].Get<uint32_t>();

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
            if (!IsBot(candidate))
                continue;
            if (candidate->GetTeamId() != player->GetTeamId())
                continue;
            if (candidate->IsInCombat() || !candidate->IsAlive())
                continue;
            if (candidate->GetDistance(player) > g_HsSayDistance)
                continue;
            if (IsBotInActiveRun(candidate->GetGUID().GetRawValue()))
                continue;
            nearbyBots.push_back(candidate);
            if (nearbyBots.size() >= 2)
                break;
        }

        if (nearbyBots.size() < 2)
            return;
        if (urand(0, 99) >= kScanFireChancePercent)
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
               && bot0->GetDistance(witness) <= g_HsSayDistance
               && bot1->GetDistance(witness) <= g_HsSayDistance;

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
        PlayerbotAI* speakerAI = PlayerbotsMgr::instance().GetPlayerbotAI(speaker);
        if (!speakerAI)
            return;

        // No archetype/persona goes into script generation, but the style
        // pass still runs per speaker at delivery -- the same script
        // spoken by two different bots reads as two different people.
        HsArchetype             archetype     = Hs_ArchetypeForBot(scheduled.speakerGuid, speaker->GetLevel());
        const HsArchetypeInfo&  archetypeInfo = Hs_ArchetypeInfoFor(archetype);
        HsStyleContext styleCtx;
        styleCtx.baselineCare         = archetypeInfo.care;
        styleCtx.abbrevOverrideChance = archetypeInfo.hasAbbrevOverride ? archetypeInfo.abbrevOverrideChance : -1.0f;
        styleCtx.inCombat             = false; // already confirmed not in combat above
        HsStyleResult style = Hs_ApplyStyle(scheduled.speakerGuid, speaker->GetName(), witness->GetName(), scheduled.text, styleCtx);
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
}

void HsScriptRunnerWorldScript::OnUpdate(uint32_t diff)
{
    // Fine-grained every tick -- turn pacing depends on it.
    DeliverPendingTurns();

    if (!g_HsEnable)
        return;
    HsTier ceiling = HsParseTier(g_HsMaxTierBotToBot);
    if (!HsTierAllows(ceiling, HsTier::Corpus)) // corpus-only in v1
        return;

    g_ScanAccumulatorMs += diff;
    if (g_ScanAccumulatorMs < kScanIntervalMs)
        return;
    g_ScanAccumulatorMs = 0;

    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        Player* player = itr.second;
        if (!player || !player->IsInWorld() || IsBot(player))
            continue;
        TryFireNearPlayer(player);
    }
}

void Hs_AbortScriptsWitnessedBy(uint64_t playerGuid)
{
    std::lock_guard<std::mutex> lock(g_RunsMutex);
    for (auto& entry : g_ActiveRuns)
        if (entry.second.witnessGuid == playerGuid)
            entry.second.aborted = true;
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
