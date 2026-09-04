#include "hs_engagement.h"
#include "hs_config.h"
#include "hs_opener.h"
#include "hs_queue.h"
#include "hs_tier.h"
#include "hs_topic_gate.h"
#include "hs_locale.h"

#include "DBCStores.h"
#include "Group.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h" // NewRpgStatus
#include "PlayerbotMgr.h"
#include "Random.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    // Starting-guess compiled constants, same reasoning as hs_opener.cpp's
    // kOpenerCooldownSeconds/kOpenerFireChancePercent: tuned later
    // against live-realm evidence, not fixed here.
    constexpr uint32_t kEngagementScanIntervalMs        = 30000; // matches hs_script.cpp's cadence
    constexpr int64_t  kEngagementFireWindowMinSec       = 60;
    constexpr int64_t  kEngagementFireWindowMaxSec       = 90;
    constexpr uint32_t kEngagementBaseFireChancePercent  = 85;   // depth 0
    constexpr uint32_t kEngagementDecayRatioPercent      = 72;   // multiplier per additional depth
    constexpr uint32_t kEngagementMaxChainDepth          = 7;    // safety valve, not the normal stop condition
    constexpr int64_t  kEngagementStaleSeconds           = 900;  // untouched this long -> forgotten, next chain starts fresh

    // A follow-up has no real player line to answer. This is the
    // "trigger" passed to Hs_CallLLM in its place, the same way a normal
    // reply's trigger is the player's chat text. History (read as context,
    // never written back to for a follow-up, see hs_queue.cpp) still gives
    // the model the real prior exchange to continue.
    const char* kEngagementFollowUpTrigger =
        "Continue the conversation naturally with a brief follow-up of your own -- "
        "a question back, or a related comment. Don't repeat what was just said.";

    struct EngagementPairState
    {
        Clock::time_point lastActivityAt;
        bool              isWhisper;
        bool              eligible;
        uint32_t          chainDepth;
    };

    std::mutex g_EngagementMutex;
    std::map<std::pair<uint64_t, uint64_t>, EngagementPairState> g_EngagementState;

    std::atomic<uint32_t> g_EngagementFollowUpsFiredThisSession{0};

    struct FireCandidate
    {
        uint64_t botGuid;
        uint64_t senderGuid;
        bool     isWhisper;
        uint32_t chainDepth;
    };

    // Prunes stale pairs and collects candidates currently inside the fire
    // window, snapshotting just enough to act on without holding the lock
    // across ObjectAccessor lookups / Hs_TryEnqueue below (same "lock only
    // long enough to read/write the map" discipline hs_queue.cpp's
    // HistorySnapshot uses).
    std::vector<FireCandidate> CollectFireCandidates()
    {
        std::vector<FireCandidate> candidates;
        Clock::time_point now = Clock::now();

        std::lock_guard<std::mutex> lock(g_EngagementMutex);
        for (auto it = g_EngagementState.begin(); it != g_EngagementState.end(); )
        {
            auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastActivityAt).count();
            if (elapsedSec >= kEngagementStaleSeconds)
            {
                it = g_EngagementState.erase(it);
                continue;
            }

            if (it->second.eligible && elapsedSec >= kEngagementFireWindowMinSec && elapsedSec < kEngagementFireWindowMaxSec
                && it->second.chainDepth < kEngagementMaxChainDepth)
            {
                candidates.push_back({ it->first.first, it->first.second, it->second.isWhisper, it->second.chainDepth });
            }

            ++it;
        }
        return candidates;
    }

    // Marks a fired follow-up's outcome on its pair state: eligible=false
    // (the next follow-up in this chain needs another direct reply first),
    // depth+1, lastActivityAt refreshed so the pair doesn't immediately
    // re-enter the fire window.
    void MarkFollowUpFired(uint64_t botGuid, uint64_t senderGuid)
    {
        std::lock_guard<std::mutex> lock(g_EngagementMutex);
        auto it = g_EngagementState.find({ botGuid, senderGuid });
        if (it == g_EngagementState.end())
            return; // aborted/evicted between candidate collection and firing
        it->second.eligible     = false;
        it->second.chainDepth  += 1;
        it->second.lastActivityAt = Clock::now();
    }

    void TryFireFollowUp(const FireCandidate& candidate)
    {
        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(candidate.botGuid));
        if (!bot || !bot->IsInWorld())
            return;
        Player* sender = ObjectAccessor::FindPlayer(ObjectGuid(candidate.senderGuid));
        if (!sender || !sender->IsInWorld())
            return;

        if (!candidate.isWhisper)
        {
            // Same eligibility a normal /say reply already uses
            // (hs_handler.cpp): a follow-up on a public line the bot can
            // no longer actually be heard on doesn't get to fire regardless.
            if (bot->GetTeamId() != sender->GetTeamId())
                return;
            if (!bot->IsWithinDistInMap(sender, g_HsSayDistance))
                return;
        }

        if (g_HsDisableRepliesInCombat && bot->IsInCombat())
            return;

        double chancePercent = kEngagementBaseFireChancePercent;
        for (uint32_t i = 0; i < candidate.chainDepth; ++i)
            chancePercent *= (kEngagementDecayRatioPercent / 100.0);
        if (urand(0, 99) >= static_cast<uint32_t>(chancePercent))
            return;

        uint64_t botGuid    = candidate.botGuid;
        uint64_t senderGuid = candidate.senderGuid;
        bool     inCombat   = bot->IsInCombat();
        uint8_t  botLevel   = bot->GetLevel();

        NewRpgStatus rpgStatus = RPG_IDLE;
        if (PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
            rpgStatus = botAI->rpgInfo.GetStatus();

        // §4.13's remaining topic-gate facts, same read as
        // hs_handler.cpp's TryDispatch, duplicated here rather than shared
        // since inCombat/botLevel/rpgStatus above already follow that
        // per-call-site pattern.
        HsTopicGateContext topicGate;
        topicGate.avgItemLevel = static_cast<uint32_t>(bot->GetAverageItemLevel());
        if (Group* group = bot->GetGroup())
        {
            topicGate.inGroup       = true;
            topicGate.isGroupLeader = group->IsLeader(bot->GetGUID());
        }
        if (Map* map = bot->GetMap())
        {
            topicGate.inInstance = map->IsDungeon() || map->IsRaid();
            if (topicGate.inInstance)
                topicGate.instanceName = map->GetMapName();
        }
        topicGate.goldCopper = bot->GetMoney();
        if (AreaTableEntry const* entry = sAreaTableStore.LookupEntry(bot->GetZoneId()))
        {
            std::string zoneName = Hs_LocalizedAreaName(entry); // review H1
            if (!zoneName.empty())
                topicGate.zoneName = zoneName;
        }

        bool admitted = Hs_TryEnqueue(botGuid, bot->GetName(), senderGuid, sender->GetName(),
            candidate.isWhisper ? HsReplyChannel::Whisper : HsReplyChannel::Say,
            kEngagementFollowUpTrigger, inCombat, botLevel, rpgStatus, topicGate,
            /*isFollowUp=*/true);
        if (!admitted)
            return; // same bucket/cooldown/breaker/queue-depth gates as any reply: silence, not a retry

        MarkFollowUpFired(botGuid, senderGuid);
        g_EngagementFollowUpsFiredThisSession.fetch_add(1);
    }

    void ScanEngagementFollowUps()
    {
        // Stale-purge and candidate collection happen regardless of the
        // ceiling below, so tracked state stays bounded even while the
        // feature is off; only the actual firing is ceiling-gated.
        std::vector<FireCandidate> candidates = CollectFireCandidates();
        if (candidates.empty())
            return;

        HsTier ceiling = HsParseTier(g_HsMaxTierEngagementFollowUp);
        if (!HsTierAllows(ceiling, HsTier::Inference))
            return;

        for (auto const& candidate : candidates)
            TryFireFollowUp(candidate);
    }
}

void Hs_EngagementNoteDirectReply(uint64_t botGuid, uint64_t senderGuid, bool isWhisper)
{
    std::lock_guard<std::mutex> lock(g_EngagementMutex);
    EngagementPairState& state = g_EngagementState[{ botGuid, senderGuid }];
    state.lastActivityAt = Clock::now();
    state.isWhisper      = isWhisper;
    state.eligible       = true;
    // chainDepth deliberately untouched: a genuine direct reply re-arms
    // eligibility for the next follow-up without resetting how deep this
    // conversation's chain already is.
}

void Hs_AbortEngagementFollowUpsFor(uint64_t playerGuid)
{
    {
        std::lock_guard<std::mutex> lock(g_EngagementMutex);
        for (auto& entry : g_EngagementState)
            if (entry.first.second == playerGuid)
                entry.second.eligible = false;
    }
    Hs_CancelPendingFollowUpsFor(playerGuid);
}

uint32_t Hs_EngagementFollowUpsFiredThisSession()
{
    return g_EngagementFollowUpsFiredThisSession.load();
}

void HsEngagementScanWorldScript::OnUpdate(uint32_t diff)
{
    static uint32_t s_AccumulatorMs = 0;
    s_AccumulatorMs += diff;
    if (s_AccumulatorMs < kEngagementScanIntervalMs)
        return;
    // Subtract rather than reset: the fire window
    // [kEngagementFireWindowMinSec, kEngagementFireWindowMaxSec) is exactly
    // one scan interval wide, so it only reliably catches every pair while
    // the scan period stays exactly kEngagementScanIntervalMs. Discarding the
    // per-cycle overshoot (up to one world tick) would drift the period past
    // 30s and eventually step a pair's window entirely: the follow-up would
    // never fire and nothing would record why.
    s_AccumulatorMs -= kEngagementScanIntervalMs;

    if (!g_HsEnable)
        return;

    ScanEngagementFollowUps();

    // hs_opener.h's own named fifth trigger ("prolonged proximity"),
    // sharing this tick rather than running a second near-identical scan
    // timer.
    Hs_ScanProximityOpeners();
}
