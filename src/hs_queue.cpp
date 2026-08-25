#include "hs_queue.h"
#include "hs_archetype.h"
#include "hs_botchain.h"
#include "hs_config.h"
#include "hs_engagement.h"
#include "hs_identity.h"
#include "hs_identity_store.h"
#include "hs_llm.h"
#include "hs_memory_store.h"
#include "hs_style.h"

#include "Channel.h"          // §4.17 channel delivery: Channel::Say
#include "ChannelMgr.h"       // §4.17 channel delivery: ChannelMgr::forTeam/GetChannel
#include "DBCStores.h"        // §4.17 channel delivery: sChatChannelsStore (zone-qualified channel name)
#include "DatabaseEnv.h" // HearthsideChat.DebugChatLog insert
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"      // also mod-playerbots' ChatChannelId enum, reused for §4.17's DBC id mapping
#include "PlayerbotMgr.h"
#include "ObjectAccessor.h"
#include "Random.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    // Self-correction follow-up: a settled design constant, not an operator
    // knob, so it lives here rather than in hs_config.h.
    constexpr uint32_t kSelfCorrectionChancePercent   = 5;
    constexpr uint32_t kSelfCorrectionMinDelaySeconds = 2;
    constexpr uint32_t kSelfCorrectionMaxDelaySeconds = 5;

    // A short, factual line describing what the bot is actually doing right
    // now (mod-playerbots' own NewRpgStatus), appended to personaLine in
    // WorkerLoop below so a free-generating reply can't contradict
    // observable state -- e.g. claiming to be PvPing while mid-quest. Stated
    // as plain fact, not an instruction, matching how the card voice block
    // and grounded answers address the model. RPG_IDLE and the wander states
    // return empty -- not distinctive enough to be worth stating.
    std::string RpgStatusHint(NewRpgStatus status)
    {
        switch (status)
        {
            case RPG_GO_GRIND:      return "Right now you're out grinding mobs.";
            case RPG_GO_CAMP:       return "Right now you're camping a spot, waiting for something to spawn.";
            case RPG_DO_QUEST:      return "Right now you're in the middle of a quest.";
            case RPG_TRAVEL_FLIGHT: return "Right now you're traveling.";
            case RPG_REST:          return "Right now you're taking a break in town.";
            case RPG_OUTDOOR_PVP:   return "Right now you're fighting over a PvP objective.";
            default:                return ""; // RPG_IDLE, RPG_WANDER_RANDOM, RPG_WANDER_NPC -- not distinctive enough to state
        }
    }

    // Real players answer these in under a second, but a same-tick reply
    // would itself be a tell, so tier 0 still gets a short randomized delay.
    constexpr uint32_t kReflexDelayMinMs = 400;
    constexpr uint32_t kReflexDelayMaxMs = 1500;

    // The distracted-reply filler pool (hs_config.h's Distracted.* section).
    // Hardcoded content rather than a config key or an SQL table, following
    // the same reasoning hs_reflex.h's gz/ty/inv vocabulary already gets:
    // editing costs a rebuild, which is the right price for a fixed set this
    // small. Written lowercase/uncapitalized as neutral raw input -- each
    // pick still goes through the same style pass (WorkerLoop below, same
    // HsStyleContext as the real reply) before delivery, so a careful
    // archetype's casing/punctuation and a carded verbal tic still apply;
    // this pool is not what a player actually sees verbatim.
    constexpr std::array<const char*, 8> kDistractedFillers = {{
        "sorry, was afk",
        "back",
        "sorry, back",
        "back, sorry",
        "sry was afk",
        "ok im back",
        "was away",
        "was afk"
    }};

    const char* ReplyChannelNameImpl(HsReplyChannel channel)
    {
        switch (channel)
        {
            case HsReplyChannel::Say:     return "say";
            case HsReplyChannel::Whisper: return "whisper";
            case HsReplyChannel::Party:   return "party";
            case HsReplyChannel::Raid:    return "raid";
            case HsReplyChannel::Guild:   return "guild";
            case HsReplyChannel::Channel: return "channel";
        }
        return "say";
    }

    // hs_identity.h's weight table, keyed by delivery channel (§4.12).
    // Channel (§4.17) scores nothing -- ambient chatter, not addressed at a
    // player, same rule openers/engagement follow-ups already get.
    uint32_t ScoreWeightForChannel(HsReplyChannel channel)
    {
        switch (channel)
        {
            case HsReplyChannel::Whisper: return kHsScoreWeightWhisper;
            case HsReplyChannel::Party:
            case HsReplyChannel::Raid:    return kHsScoreWeightPartyRaid;
            case HsReplyChannel::Guild:   return kHsScoreWeightGuild;
            case HsReplyChannel::Say:     return kHsScoreWeightSay;
            case HsReplyChannel::Channel: return 0;
        }
        return kHsScoreWeightSay;
    }

    struct HsQueuedRequest
    {
        uint64_t     botGuid;
        std::string  botName;     // style pass: protected from typo injection
        uint64_t     senderGuid;
        std::string  senderName;  // style pass: protected from typo injection
        HsReplyChannel channel;
        std::string  prompt;
        Clock::time_point enqueuedAt;
        bool         isProbe;
        bool         inCombat;    // style pass: combat `care` offset
        uint8_t      botLevel;    // archetype eligibility filter (hs_archetype.h)
        NewRpgStatus rpgStatus;   // live activity fact, folded into personaLine below
        HsTopicGateContext topicGate; // §4.13 gear/group/instance/gold/zone facts, folded into personaLine below
        bool         isFollowUp;  // self-initiated engagement follow-up (hs_engagement.h) -- no score, no history write
        bool         isEvent;     // event reaction (hs_event.h) -- as isFollowUp, plus no first-meeting record
        HsChannelKind channelKind = HsChannelKind::Trade; // meaningful only when channel == HsReplyChannel::Channel (§4.17)
        uint64_t     chainScopeId = 0; // bot-to-bot chain hop (hs_botchain.h); 0 = not a hop
        uint32_t     chainSeq     = 0; // the scope generation this hop was issued under
    };

    // deliverAt lets one worker thread queue two chat lines instead of one:
    // the reply after its typing delay, and a self-correction `*correction`
    // a beat after that. Reflex/grounded/corpus-fallback replies (delivered
    // via Hs_DeliverReflexReply, not this struct's other producer below)
    // still use their own fixed 400-1500ms window regardless of text length.
    struct HsPendingReply
    {
        uint64_t    botGuid;
        uint64_t    senderGuid;
        HsReplyChannel channel;
        std::string text;
        Clock::time_point deliverAt;
        bool        isFollowUp; // cancellable via Hs_CancelPendingFollowUpsFor; direct replies and the self-correction addendum are never tagged
        HsChannelKind channelKind = HsChannelKind::Trade; // meaningful only when channel == HsReplyChannel::Channel (§4.17)
        uint64_t    chainScopeId = 0; // bot-to-bot chain hop (hs_botchain.h); 0 = not a hop, which is every other producer
        uint32_t    chainSeq     = 0; // scope generation at issue time; a newer generation means a player took the floor -- drop
        // Whether this line may seed the next hop of a bot-to-bot chain.
        // False for the two secondary lines a single reply can also queue --
        // the distracted "sorry, was afk" filler and the `*correction`
        // addendum. Both are flavor attached to the primary reply, and
        // neither is something another bot should be answering: a hop
        // triggered by a bare "*healer" fragment has no conversation in it.
        bool        seedsChain = true;
    };

    // ---- work queue: world thread pushes, the one worker thread pops ----
    std::mutex                     g_QueueMutex;
    std::condition_variable        g_QueueCv;
    std::deque<HsQueuedRequest>    g_Queue;
    bool                            g_StopWorker = false;
    std::thread                     g_WorkerThread;

    // ---- delivery queue: worker thread pushes, world thread pops (Hs_DeliverPending) ----
    std::mutex                     g_DeliveryMutex;
    std::deque<HsPendingReply>     g_DeliveryQueue;

    // ---- token bucket: the primary load ceiling ----
    std::mutex          g_BucketMutex;
    double               g_BucketTokens = 0.0;
    Clock::time_point    g_BucketLastRefill;
    bool                 g_BucketInitialized = false;

    // ---- §4.17: one token bucket per channel, independent of the tier-2
    // bucket above -- corpus-fallback channel replies never touch that one
    // (zero GPU work). Burst capacity equals the channel's own RatePerMin
    // (a channel can spend a full minute's budget at once, then waits), no
    // separate config key. Same shape as the tier-2 bucket, keyed by channel
    // instead of global, one shared mutex since writes are rare (one
    // channel message at a time, never hot enough to need per-key locking).
    struct HsChannelBucketState
    {
        double            tokens = 0.0;
        Clock::time_point lastRefill;
        bool              initialized = false;
    };
    std::mutex                                            g_ChannelBucketMutex;
    std::unordered_map<HsChannelKind, HsChannelBucketState> g_ChannelBuckets;

    // ---- PLAN-ARBITER.md §8: the event tier's own bucket, independent of
    // the tier-2 bucket above so ambient event reactions can never spend the
    // budget a player's /say needed. Same lazy-init/refill shape as both of
    // the others; its own mutex because it is taken from the world thread at
    // every event fire site, and there is no reason for a death in a dungeon
    // to wait behind a Trade-channel line.
    std::mutex        g_EventBucketMutex;
    double            g_EventBucketTokens = 0.0;
    Clock::time_point g_EventBucketLastRefill;
    bool              g_EventBucketInitialized = false;

    // ---- per-bot cooldown (gate) and last-successful-reply time (arbiter query) ----
    std::mutex                                        g_CooldownMutex;
    std::unordered_map<uint64_t, Clock::time_point>   g_LastEnqueueAt;
    std::unordered_map<uint64_t, Clock::time_point>   g_LastReplyAt;

    // ---- distracted-reply cooldown ----
    // Its own mutex rather than sharing g_CooldownMutex: that one is taken on
    // the world thread by every admission attempt (Hs_TryEnqueue), and this
    // one only by the single worker thread after a completed generation --
    // no reason to make an admission wait behind a flavor roll.
    //
    // Keyed by bot, not by (bot, player) pair: the frustration this bounds is
    // "this particular bot keeps replying late," and per-bot is also what the
    // per-bot reply cooldown above already keys on. A player talking to many
    // bots can still see two distracted replies close together from two
    // different bots, which is the intended read -- two people happened to
    // step away, not one flaky bot.
    std::mutex                                        g_DistractedMutex;
    std::unordered_map<uint64_t, Clock::time_point>   g_LastDistractedAt;

    // Rolls this reply's distracted flavor and, on a hit, claims the bot's
    // cooldown window in the same call -- returning true means the caller
    // *will* deliver a filler line, so the claim can't be deferred.
    //
    // The chance roll runs before the cooldown check so the cheap test
    // short-circuits ahead of the lock; a roll that hits while the bot is
    // still cooling down is simply discarded, not banked.
    bool TryClaimDistractedReply(uint64_t botGuid, float chance)
    {
        if (!g_HsDistractedEnabled)
            return false;

        uint32_t chancePct = static_cast<uint32_t>(std::clamp(chance, 0.0f, 1.0f) * 100.0f);
        if (chancePct == 0 || urand(1, 100) > chancePct)
            return false;

        Clock::time_point now = Clock::now();

        std::lock_guard<std::mutex> lock(g_DistractedMutex);
        auto it = g_LastDistractedAt.find(botGuid);
        if (it != g_LastDistractedAt.end())
        {
            auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
            if (elapsedSec < static_cast<int64_t>(g_HsDistractedCooldownSeconds))
                return false;
        }

        g_LastDistractedAt[botGuid] = now;
        return true;
    }

    // ---- conversation history: a few lines per bot-player pair, held in
    // memory only. Keyed by (botGuid, senderGuid); std::map's operator< on
    // std::pair needs no custom hash. ----
    std::mutex g_HistoryMutex;
    std::map<std::pair<uint64_t, uint64_t>, std::deque<HsHistoryTurn>> g_History;

    // Snapshot for the worker thread to hand to Hs_CallLLM. Returns a copy so
    // the lock is held only long enough to read it.
    std::vector<HsHistoryTurn> HistorySnapshot(uint64_t botGuid, uint64_t senderGuid)
    {
        if (g_HsLLMHistoryTurns == 0)
            return {};
        std::lock_guard<std::mutex> lock(g_HistoryMutex);
        auto it = g_History.find({ botGuid, senderGuid });
        if (it == g_History.end())
            return {};
        return { it->second.begin(), it->second.end() };
    }

    // Stores the exact trigger/reply just used, byte-for-byte, then evicts
    // from the front until at most g_HsLLMHistoryTurns pairs remain.
    void HistoryAppend(uint64_t botGuid, uint64_t senderGuid, const std::string& trigger, const std::string& reply)
    {
        if (g_HsLLMHistoryTurns == 0)
            return;
        std::lock_guard<std::mutex> lock(g_HistoryMutex);
        std::deque<HsHistoryTurn>& turns = g_History[{ botGuid, senderGuid }];
        turns.push_back({ trigger, reply });
        while (turns.size() > g_HsLLMHistoryTurns)
            turns.pop_front();
    }

    // ---- circuit breaker ----
    std::atomic<uint32_t>  g_ConsecutiveFailures{0};
    std::atomic<bool>      g_BreakerOpen{false};
    std::atomic<bool>      g_ProbeInFlight{false};
    std::mutex             g_ProbeMutex;
    Clock::time_point      g_LastProbeAt;
    bool                    g_HasProbedOnce = false;

    // ---- idle signal for the generator: true only while the worker thread
    // is actually inside Hs_CallLLM ----
    std::atomic<bool> g_ReactiveWorkerBusy{false};

    // ---- `.hearthside capture`: last pre-style reply per bot name.
    // Overwritten on every successful reactive reply; not a history. ----
    std::mutex                                  g_LastPreStyleMutex;
    std::unordered_map<std::string, std::string> g_LastPreStyleReplyByBot;

    // ---- §4.19 fuller metrics: rolling latency samples, prompt-char sums
    // by ring, reply/silence counts by archetype and channel. One mutex
    // covers all four -- updates are cheap and never contended against
    // anything but this same worker thread's next request. ----
    std::mutex           g_MetricsMutex;
    std::deque<uint32_t> g_LatencySamplesMs;
    constexpr size_t     kMaxLatencySamples = 500; // a rolling window, not a full history -- hside_metrics is the history

    struct RingPromptStats { uint64_t sumChars = 0; uint32_t count = 0; };
    RingPromptStats g_PromptStatsByRing[3]; // index 0 = ring 1, 1 = ring 2, 2 = ring 3

    struct ReplyCounts { uint32_t replied = 0; uint32_t silent = 0; };
    std::unordered_map<std::string, ReplyCounts> g_ReplyCountsByArchetype;
    std::unordered_map<uint8_t, ReplyCounts>     g_ReplyCountsByChannel;

    // TTL-drop and token-bucket-saturation counts, session-cumulative like
    // the reply/silence counts above -- an operator-visible answer to "is
    // the queue going stale" / "is a rate limit actually binding", named as
    // a gap in Claude/ISSUES.md while building the rest of §4.19's metrics.
    uint64_t g_TtlDroppedSession    = 0;
    uint64_t g_TtlProcessedSession  = 0; // dropped + handled -- the drop-rate denominator
    uint64_t g_BucketDeniedSession   = 0;
    uint64_t g_BucketAttemptedSession = 0;
    std::unordered_map<HsChannelKind, ReplyCounts> g_ChannelBucketCountsByKind; // .replied = granted, .silent = denied

    void RecordLatencySample(uint32_t ms)
    {
        std::lock_guard<std::mutex> lock(g_MetricsMutex);
        g_LatencySamplesMs.push_back(ms);
        while (g_LatencySamplesMs.size() > kMaxLatencySamples)
            g_LatencySamplesMs.pop_front();
    }

    void RecordPromptChars(uint8_t ring, uint32_t chars)
    {
        if (ring < 1 || ring > 3)
            return;
        std::lock_guard<std::mutex> lock(g_MetricsMutex);
        RingPromptStats& stats = g_PromptStatsByRing[ring - 1];
        stats.sumChars += chars;
        ++stats.count;
    }

    void RecordRequestOutcome(const std::string& archetypeName, HsReplyChannel channel, bool replied)
    {
        std::lock_guard<std::mutex> lock(g_MetricsMutex);
        ReplyCounts& byArchetype = g_ReplyCountsByArchetype[archetypeName];
        ReplyCounts& byChannel   = g_ReplyCountsByChannel[static_cast<uint8_t>(channel)];
        if (replied)
        {
            ++byArchetype.replied;
            ++byChannel.replied;
        }
        else
        {
            ++byArchetype.silent;
            ++byChannel.silent;
        }
    }

    void RecordTtlOutcome(bool dropped)
    {
        std::lock_guard<std::mutex> lock(g_MetricsMutex);
        ++g_TtlProcessedSession;
        if (dropped)
            ++g_TtlDroppedSession;
    }

    void RecordBucketAttempt(bool denied)
    {
        std::lock_guard<std::mutex> lock(g_MetricsMutex);
        ++g_BucketAttemptedSession;
        if (denied)
            ++g_BucketDeniedSession;
    }

    void RecordChannelBucketAttempt(HsChannelKind kind, bool denied)
    {
        std::lock_guard<std::mutex> lock(g_MetricsMutex);
        ReplyCounts& counts = g_ChannelBucketCountsByKind[kind];
        if (denied)
            ++counts.silent;
        else
            ++counts.replied;
    }

    // Refills the bucket for elapsed time, capped at burst capacity. Caller
    // holds g_BucketMutex. Self-initializes on first call so the bucket
    // starts full regardless of Hs_QueueStartup()/config-load ordering.
    void RefillBucketLocked()
    {
        Clock::time_point now = Clock::now();
        if (!g_BucketInitialized)
        {
            g_BucketTokens      = static_cast<double>(g_HsBucketBurstCapacity);
            g_BucketLastRefill  = now;
            g_BucketInitialized = true;
            return;
        }

        double elapsedSec = std::chrono::duration<double>(now - g_BucketLastRefill).count();
        g_BucketLastRefill = now;

        double ratePerSec = static_cast<double>(g_HsBucketRepliesPerMinute) / 60.0;
        g_BucketTokens = std::min(static_cast<double>(g_HsBucketBurstCapacity), g_BucketTokens + elapsedSec * ratePerSec);
    }

    // Updates the consecutive-failure count and flips the breaker open/closed
    // as needed. Edge-triggered logging only -- one line per transition,
    // never per request; the breaker stays silent while open.
    void RecordOutcome(bool success)
    {
        if (success)
        {
            g_ConsecutiveFailures.store(0);
            if (g_BreakerOpen.exchange(false))
                LOG_INFO("server.loading", "[HearthsideChat] Circuit breaker closed - backend recovered.");
            return;
        }

        uint32_t failures = g_ConsecutiveFailures.fetch_add(1) + 1;
        if (failures >= g_HsBreakerFailureThreshold && !g_BreakerOpen.exchange(true))
            LOG_ERROR("server.loading", "[HearthsideChat] Circuit breaker opened after {} consecutive failures.", failures);
    }

    void WorkerLoop()
    {
        for (;;)
        {
            HsQueuedRequest req;
            {
                std::unique_lock<std::mutex> lock(g_QueueMutex);
                g_QueueCv.wait(lock, [] { return g_StopWorker || !g_Queue.empty(); });
                if (g_StopWorker && g_Queue.empty())
                    return;
                req = std::move(g_Queue.front());
                g_Queue.pop_front();
            }

            auto ageSec = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - req.enqueuedAt).count();
            if (ageSec > static_cast<int64_t>(g_HsQueueTTLSeconds))
            {
                RecordTtlOutcome(/*dropped=*/true);
                if (req.isProbe)
                    g_ProbeInFlight.store(false);
                if (g_HsDebugEnabled)
                    LOG_INFO("server.loading", "[HearthsideChat] Dropping stale request for bot {} (age {}s > TTL {}s).",
                        req.botGuid, ageSec, g_HsQueueTTLSeconds);
                continue;
            }
            RecordTtlOutcome(/*dropped=*/false);

            // The bot's archetype, drawn deterministically from its GUID and
            // restricted to the level-eligible pool (hs_archetype.h). Feeds
            // both the LLM prompt's delta layer and the style pass's `care`
            // baseline below.
            HsArchetype archetype = Hs_ArchetypeForBot(req.botGuid, req.botLevel);
            HsArchetypeInfo const archetypeInfo = Hs_ArchetypeInfoFor(archetype);

            // The voice block is the only card text that ever enters a
            // prompt. Folded into the same personaLine string Hs_CallLLM
            // already takes, after the archetype line and before history,
            // rather than widening that function's signature -- a no-op
            // concat for the majority of bots with no active card.
            HsCardSnapshot cardSnapshot = Hs_LookupCardSnapshot(req.botGuid);

            // Ring, derived the same way hs_identity.h's table defines it
            // (card_active -> 3, else has memory rows -> 2, else -> 1).
            // Feeds only the §4.19 prompt-length-by-ring metric below; the
            // prompt itself doesn't change shape by ring.
            uint8_t ring = cardSnapshot.active ? 3
                : (Hs_HasMetBefore(req.botGuid, req.senderGuid) ? 2 : 1);

            std::string personaLine = Hs_ArchetypePromptLine(archetype);
            if (cardSnapshot.active && !cardSnapshot.voiceBlock.empty())
                personaLine += "\n" + cardSnapshot.voiceBlock;
            std::string rpgHint = RpgStatusHint(req.rpgStatus);
            if (!rpgHint.empty())
                personaLine += "\n" + rpgHint;
            personaLine += "\n" + Hs_TopicGateLine(req.topicGate);

            HsLLMConfig cfg;
            cfg.apiType       = g_HsLLMApiType;
            cfg.baseUrl       = g_HsLLMUrl;
            cfg.model         = g_HsLLMModel;
            cfg.apiKey        = g_HsLLMApiKey;
            cfg.timeoutSec    = static_cast<int>(g_HsLLMTimeoutSeconds);
            cfg.templateKind  = g_HsLLMTemplate;
            // Per-archetype verbosity cap refines the operator's configured
            // ceiling downward; it never raises it above what
            // HearthsideChat.LLM.MaxTokens allows.
            cfg.maxTokens     = static_cast<int>(std::min(g_HsLLMMaxTokens, archetypeInfo.verbosityCap));
            cfg.dryMultiplier = g_HsLLMDryMultiplier;

            std::vector<HsHistoryTurn> history = HistorySnapshot(req.botGuid, req.senderGuid);

            g_ReactiveWorkerBusy.store(true);
            HsLLMResult result = Hs_CallLLM(cfg, g_HsLLMSystemPrompt, personaLine, history, req.prompt);
            g_ReactiveWorkerBusy.store(false);

            RecordOutcome(result.success);
            if (req.isProbe)
                g_ProbeInFlight.store(false);

            // §4.19: latency and prompt length are meaningful on every
            // outcome, not just a delivered reply -- a failing backend
            // should show up in the latency percentiles, not silently drop
            // out of them.
            RecordLatencySample(result.latencyMs);
            RecordPromptChars(ring, result.promptChars);

            if (!result.success || result.text.empty())
            {
                if (g_HsDebugEnabled)
                    LOG_INFO("server.loading", "[HearthsideChat] No reply for bot {} (failure={}, httpStatus={}).",
                        req.botGuid, static_cast<int>(result.failure), result.httpStatus);
                RecordRequestOutcome(archetypeInfo.enumName, req.channel, /*replied=*/false);
                continue; // silence, not a canned fallback
            }

            // `.hearthside capture`: stash the pre-style text before the
            // style pass below reshapes it, so what a GM captures is the
            // model's clean output, not this reply's injected typos.
            {
                std::lock_guard<std::mutex> lock(g_LastPreStyleMutex);
                g_LastPreStyleReplyByBot[req.botName] = result.text;
            }

            // Caps/punctuation/abbrev reshaping and typo injection, applied
            // before the text becomes either delivered chat or the next
            // turn's history line -- so a re-rendered history line and the
            // one actually spoken always match byte-for-byte. `care`'s
            // baseline is the archetype's; TRADER is the only entry with an
            // abbreviation override today.
            HsStyleContext styleCtx;
            styleCtx.baselineCare         = archetypeInfo.care;
            styleCtx.abbrevOverrideChance = archetypeInfo.hasAbbrevOverride ? archetypeInfo.abbrevOverrideChance : -1.0f;
            styleCtx.inCombat             = req.inCombat;
            styleCtx.verbalTic            = cardSnapshot.verbalTic;
            styleCtx.tradeCareOffset      = Hs_TradeCareOffsetFor(req.botGuid); // §4.17: no Player* needed, safe off-thread
            // Captured before result.text is overwritten below, so
            // HearthsideChat.DebugChatLog can log both the pre-style and
            // post-style text for an operator to review later.
            std::string preStyleForLog = result.text;

            HsStyleResult style = Hs_ApplyStyle(req.botGuid, req.botName, req.senderName, result.text, styleCtx);
            result.text = style.text;
            if (result.text.empty())
            {
                RecordRequestOutcome(archetypeInfo.enumName, req.channel, /*replied=*/false);
                continue;
            }
            RecordRequestOutcome(archetypeInfo.enumName, req.channel, /*replied=*/true);

            if (g_HsDebugChatLogEnabled)
            {
                std::string escapedBotName    = req.botName;    CharacterDatabase.EscapeString(escapedBotName);
                std::string escapedSenderName = req.senderName; CharacterDatabase.EscapeString(escapedSenderName);
                std::string escapedArchetype  = archetypeInfo.enumName; CharacterDatabase.EscapeString(escapedArchetype);
                std::string escapedTrigger    = req.prompt;     CharacterDatabase.EscapeString(escapedTrigger);
                std::string escapedPreStyle   = preStyleForLog; CharacterDatabase.EscapeString(escapedPreStyle);
                std::string escapedStyled     = result.text;    CharacterDatabase.EscapeString(escapedStyled);
                CharacterDatabase.Execute(
                    "INSERT INTO hside_chat_log (bot_guid, bot_name, sender_guid, sender_name, is_whisper, "
                    "archetype, trigger_text, pre_style_text, styled_text, created_at) "
                    "VALUES ({}, '{}', {}, '{}', {}, '{}', '{}', '{}', '{}', NOW())",
                    req.botGuid, escapedBotName, req.senderGuid, escapedSenderName,
                    req.channel == HsReplyChannel::Whisper ? 1 : 0, // party/raid/guild collapse to 0 -- debug log doesn't distinguish them from say yet
                    escapedArchetype, escapedTrigger, escapedPreStyle, escapedStyled);
            }

            // The two bot-initiated request kinds share every "this was not
            // a player utterance" suppression below. Named once here so a
            // third kind can't accidentally pick up only some of them.
            bool botInitiated = req.isFollowUp || req.isEvent;

            // History stores only the primary reply, not the follow-up
            // correction below -- a bare "*healer" fragment isn't useful
            // prior-turn context, and the corrected meaning is already
            // fully present in result.text. An engagement follow-up
            // (hs_engagement.h) is skipped for the same reason: its
            // "trigger" is a synthetic instruction, not something the
            // player actually said. An event trigger (hs_event.h) is a
            // synthetic state line for the same reason -- nobody said
            // "you have just been killed" to the bot.
            if (!botInitiated)
                HistoryAppend(req.botGuid, req.senderGuid, req.prompt, result.text);

            // Scores the bot the arbiter selected, only when the reply
            // resolves to tier 2 -- reflex/grounded/corpus-fallback replies
            // never reach here. An engagement follow-up or event reaction is
            // bot-initiated, not a scored player utterance, same as
            // bot-initiated openers.
            if (!botInitiated)
                Hs_BumpInteractionScore(req.botGuid, req.botLevel, ScoreWeightForChannel(req.channel));

            // Re-arms this (bot, player) pair's engagement-follow-up
            // eligibility -- only a genuine direct reply does this, never a
            // follow-up's own delivery, so a chain only continues as long
            // as the player keeps replying. Engagement follow-up is a
            // whisper/say-only surface (§4.22); a party/raid/guild reply
            // doesn't arm it.
            if (!botInitiated && (req.channel == HsReplyChannel::Say || req.channel == HsReplyChannel::Whisper))
                Hs_EngagementNoteDirectReply(req.botGuid, req.senderGuid, req.channel == HsReplyChannel::Whisper);

            // Ordinary chat is the most common way two people actually meet,
            // so first-meeting is seeded here rather than only as a side
            // effect of the rarer shared-experience events (dungeon/group/
            // death/guild). Idempotent -- a no-op after the pair's first.
            //
            // Skipped for an event reaction, and only for that: an event's
            // senderGuid is whoever it happened around, which is frequently
            // another bot (a bot's own death, a bot-only stretch of a
            // group), and "met" between two bots is not a fact about any
            // player. An engagement follow-up still records it -- its sender
            // is by construction the real player it is following up with.
            if (!req.isEvent)
                Hs_EnsureFirstMeetingRecorded(req.botGuid, req.senderGuid);

            {
                std::lock_guard<std::mutex> lock(g_CooldownMutex);
                g_LastReplyAt[req.botGuid] = Clock::now();
            }

            Clock::time_point now = Clock::now();

            // Typing delay for the tier-2 reply: a residual on top of
            // however long Hs_CallLLM already took, so the total (real
            // generation latency + top-up) approximates a human typing the
            // reply without ever shortening what the LLM call itself cost.
            // A fast backend that would otherwise deliver same-tick still
            // gets the full target delay.
            int64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - req.enqueuedAt).count();

            // Hoisted out of the block below because the distracted path
            // needs the *full* target, not the residual: once the bot has
            // said "sorry, was afk" it is back at the keyboard typing the
            // whole reply from scratch, so none of the generation latency it
            // spent while "away" should count against that typing time.
            uint32_t typingTargetMs = 0;
            if (g_HsTypingDelayEnabled)
                typingTargetMs = std::min(g_HsTypingDelayMaxMs,
                    archetypeInfo.typingBaseMs + static_cast<uint32_t>(result.text.size()) * archetypeInfo.typingPerCharMs);

            Clock::time_point deliverAt = now;
            if (elapsedMs < static_cast<int64_t>(typingTargetMs))
                deliverAt = now + std::chrono::milliseconds(typingTargetMs - static_cast<uint32_t>(elapsedMs));

            // Unconditional floor on top of whatever the block above produced
            // -- closes the gap left when TypingDelay.Enable is off, where a
            // fast backend could otherwise deliver same-tick (hs_config.h).
            if (elapsedMs < static_cast<int64_t>(g_HsMinDeliveryDelayMs))
            {
                Clock::time_point floorAt = now + std::chrono::milliseconds(
                    static_cast<uint32_t>(g_HsMinDeliveryDelayMs) - static_cast<uint32_t>(elapsedMs));
                if (floorAt > deliverAt)
                    deliverAt = floorAt;
            }

            // Distracted reply: the bot "was away" for a stretch, says so,
            // then answers properly. Skipped for an engagement follow-up and
            // for an event reaction -- both are bot-initiated, so apologizing
            // for a delay to a player who never asked anything reads as a
            // non sequitur, and "sorry, was afk" in front of a reaction to
            // your own death reads worse still.
            //
            // Rolled here in the worker rather than in hs_arbiter.cpp (where
            // the retired reply_chance was rolled) for two reasons: whisper
            // never passes through the arbiter at all -- it is gated by a
            // plain chance roll in hs_handler.cpp -- and whisper is the
            // surface where this flavor reads best, so an arbiter-side roll
            // would miss it entirely; and archetypeInfo is already in hand
            // here, so nothing has to be threaded through HsQueuedRequest.
            std::string       distractedFiller;
            Clock::time_point distractedFillerAt = now;
            if (!botInitiated && TryClaimDistractedReply(req.botGuid, archetypeInfo.distractedChance))
            {
                std::string rawFiller = kDistractedFillers[urand(0, static_cast<uint32_t>(kDistractedFillers.size()) - 1)];

                // Same styleCtx as the real reply, so a careful/precise
                // archetype's filler stays properly capitalized and
                // punctuated instead of reading as sloppier than the bot
                // actually is, and a carded verbal tic/typo rate still
                // applies. Independently seeded from the real reply (the
                // hash includes the text), so the two lines don't roll
                // identically. Falls back to the raw line only in the
                // theoretical case StripLLMTells empties it.
                HsStyleResult styledFiller = Hs_ApplyStyle(req.botGuid, req.botName, req.senderName, rawFiller, styleCtx);
                distractedFiller   = styledFiller.text.empty() ? rawFiller : styledFiller.text;
                distractedFillerAt = now + std::chrono::seconds(
                    urand(g_HsDistractedMinDelaySeconds, g_HsDistractedMaxDelaySeconds));

                // The real reply lands a full typing delay after the filler,
                // never before it. MinDeliveryDelayMs still applies as the
                // floor for the case where TypingDelay.Enable is off, so the
                // two lines can never arrive on the same tick.
                deliverAt = distractedFillerAt +
                    std::chrono::milliseconds(std::max(typingTargetMs, g_HsMinDeliveryDelayMs));

                // Hold the bot's admission cooldown open until the delayed
                // reply has actually landed, by dating its last-enqueue stamp
                // into the future. Without this, Bot.CooldownSeconds (8s by
                // default) expires long before a 25-60s away window does, and
                // a second question to the same bot would be generated and
                // delivered *ahead* of the pair still sitting in the delivery
                // queue -- the bot would answer the newer question normally,
                // then say "sorry, was afk" and answer the older one. It also
                // happens to be what the fiction already claims: someone who
                // is away from the keyboard isn't answering anyone else
                // either. The gate reads `now - stamp < cooldown`, so a
                // future stamp yields a negative elapsed and stays closed.
                {
                    std::lock_guard<std::mutex> cooldownLock(g_CooldownMutex);
                    g_LastEnqueueAt[req.botGuid] = deliverAt;
                }
            }

            {
                std::lock_guard<std::mutex> lock(g_DeliveryMutex);

                // Pushed ahead of the reply so the queue reads in delivery
                // order; Hs_DeliverPending drains by deliverAt regardless.
                // channelKind/chainScopeId/chainSeq are carried on every push
                // here rather than left to HsPendingReply's defaults: a chain
                // hop (hs_botchain.h) can target HsReplyChannel::Channel, and
                // a defaulted kind would silently deliver it into Trade.
                if (!distractedFiller.empty())
                    g_DeliveryQueue.push_back({ req.botGuid, req.senderGuid, req.channel, distractedFiller,
                                                 distractedFillerAt, req.isFollowUp, req.channelKind,
                                                 req.chainScopeId, req.chainSeq, /*seedsChain=*/false });

                g_DeliveryQueue.push_back({ req.botGuid, req.senderGuid, req.channel, result.text, deliverAt,
                                             req.isFollowUp, req.channelKind, req.chainScopeId, req.chainSeq,
                                             /*seedsChain=*/true });

                // Self-correction follow-up: only eligible when a typo
                // actually landed in this message. The `*` prefix is added
                // here, after the style pass, and the corrected word is
                // exempt from it -- a plain literal fix, not another sloppy
                // line. The delay is measured from the primary reply's own
                // deliverAt (not `now`) so the correction can never arrive
                // before the line it corrects.
                if (!style.correction.empty() && urand(0, 99) < kSelfCorrectionChancePercent)
                {
                    uint32_t delaySec = urand(kSelfCorrectionMinDelaySeconds, kSelfCorrectionMaxDelaySeconds);
                    std::string followUp = "*" + style.correction;
                    g_DeliveryQueue.push_back({ req.botGuid, req.senderGuid, req.channel, followUp,
                                                 deliverAt + std::chrono::seconds(delaySec), req.isFollowUp,
                                                 req.channelKind, req.chainScopeId, req.chainSeq,
                                                 /*seedsChain=*/false });
                }
            }
        }
    }
}

void Hs_QueueStartup()
{
    std::lock_guard<std::mutex> lock(g_QueueMutex);
    g_StopWorker = false;
    if (!g_WorkerThread.joinable())
        g_WorkerThread = std::thread(WorkerLoop);
}

void Hs_QueueShutdown()
{
    {
        std::lock_guard<std::mutex> lock(g_QueueMutex);
        g_StopWorker = true;
    }
    g_QueueCv.notify_all();
    if (g_WorkerThread.joinable())
        g_WorkerThread.join();
}

bool Hs_TryEnqueue(uint64_t botGuid, const std::string& botName, uint64_t senderGuid,
                    const std::string& senderName, HsReplyChannel channel, const std::string& userPrompt,
                    bool inCombat, uint8_t botLevel, NewRpgStatus rpgStatus,
                    const HsTopicGateContext& topicGate, bool isFollowUp, bool isEvent,
                    HsChannelKind channelKind, uint64_t chainScopeId, uint32_t chainSeq)
{
    // 1. Token bucket — peek only; the spend is committed once the request
    // actually clears every later gate.
    {
        std::lock_guard<std::mutex> lock(g_BucketMutex);
        RefillBucketLocked();
        if (g_BucketTokens < 1.0)
        {
            RecordBucketAttempt(/*denied=*/true);
            return false;
        }
    }
    RecordBucketAttempt(/*denied=*/false);

    // 2. Per-bot cooldown.
    {
        std::lock_guard<std::mutex> lock(g_CooldownMutex);
        auto it = g_LastEnqueueAt.find(botGuid);
        if (it != g_LastEnqueueAt.end())
        {
            auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - it->second).count();
            if (elapsedSec < static_cast<int64_t>(g_HsBotCooldownSeconds))
                return false;
        }
    }

    // 3. Circuit breaker — silent while open, except for the single claimed
    // probe request per interval.
    bool isProbe = false;
    if (g_BreakerOpen.load())
    {
        std::lock_guard<std::mutex> lock(g_ProbeMutex);
        Clock::time_point now = Clock::now();
        bool intervalElapsed = !g_HasProbedOnce ||
            std::chrono::duration_cast<std::chrono::seconds>(now - g_LastProbeAt).count() >= static_cast<int64_t>(g_HsBreakerProbeIntervalSeconds);
        if (!intervalElapsed)
            return false;
        if (g_ProbeInFlight.exchange(true))
            return false; // another probe already claimed this interval

        g_LastProbeAt   = now;
        g_HasProbedOnce = true;
        isProbe          = true;
    }

    // 4. Bounded queue depth.
    {
        std::lock_guard<std::mutex> lock(g_QueueMutex);
        if (g_Queue.size() >= g_HsQueueMaxDepth)
        {
            if (isProbe)
                g_ProbeInFlight.store(false);
            if (g_HsDebugEnabled)
                LOG_INFO("server.loading", "[HearthsideChat] Dropping request for bot {} - queue at max depth {}.", botGuid, g_HsQueueMaxDepth);
            return false;
        }

        HsQueuedRequest req;
        req.botGuid    = botGuid;
        req.botName    = botName;
        req.senderGuid = senderGuid;
        req.senderName = senderName;
        req.channel    = channel;
        req.prompt     = userPrompt;
        req.enqueuedAt = Clock::now();
        req.isProbe    = isProbe;
        req.inCombat   = inCombat;
        req.botLevel   = botLevel;
        req.rpgStatus  = rpgStatus;
        req.topicGate  = topicGate;
        req.isFollowUp = isFollowUp;
        req.isEvent    = isEvent;
        req.channelKind  = channelKind;
        req.chainScopeId = chainScopeId;
        req.chainSeq     = chainSeq;
        g_Queue.push_back(std::move(req));
    }
    g_QueueCv.notify_one();

    // Only now commit the token spend and the cooldown timestamp — the
    // request is actually admitted.
    {
        std::lock_guard<std::mutex> lock(g_BucketMutex);
        g_BucketTokens -= 1.0;
    }
    {
        std::lock_guard<std::mutex> lock(g_CooldownMutex);
        g_LastEnqueueAt[botGuid] = Clock::now();
    }

    return true;
}

void Hs_DeliverReflexReply(uint64_t botGuid, uint64_t senderGuid, HsReplyChannel channel, const std::string& text,
                            HsChannelKind channelKind)
{
    if (text.empty())
        return;

    Clock::time_point deliverAt = Clock::now() + std::chrono::milliseconds(urand(kReflexDelayMinMs, kReflexDelayMaxMs));
    std::lock_guard<std::mutex> lock(g_DeliveryMutex);
    g_DeliveryQueue.push_back({ botGuid, senderGuid, channel, text, deliverAt, /*isFollowUp=*/false, channelKind });
}

bool Hs_EventBucketTake()
{
    if (g_HsEventBucketRepliesPerMinute == 0 || g_HsEventBucketBurstCapacity == 0)
        return false; // budget of zero is a kill switch, not an empty-then-refill

    std::lock_guard<std::mutex> lock(g_EventBucketMutex);
    Clock::time_point now = Clock::now();
    if (!g_EventBucketInitialized)
    {
        g_EventBucketTokens      = static_cast<double>(g_HsEventBucketBurstCapacity);
        g_EventBucketLastRefill  = now;
        g_EventBucketInitialized = true;
    }
    else
    {
        double elapsedSec = std::chrono::duration<double>(now - g_EventBucketLastRefill).count();
        g_EventBucketLastRefill = now;
        double ratePerSec = static_cast<double>(g_HsEventBucketRepliesPerMinute) / 60.0;
        g_EventBucketTokens = std::min(static_cast<double>(g_HsEventBucketBurstCapacity),
                                        g_EventBucketTokens + elapsedSec * ratePerSec);
    }

    if (g_EventBucketTokens < 1.0)
        return false;

    g_EventBucketTokens -= 1.0;
    return true;
}

bool Hs_ChannelBucketTake(HsChannelKind kind)
{
    uint32_t ratePerMin = Hs_ChannelPolicyFor(kind).ratePerMin;
    if (ratePerMin == 0)
        return false;

    std::lock_guard<std::mutex> lock(g_ChannelBucketMutex);
    HsChannelBucketState& state = g_ChannelBuckets[kind];

    Clock::time_point now = Clock::now();
    if (!state.initialized)
    {
        state.tokens      = static_cast<double>(ratePerMin);
        state.lastRefill  = now;
        state.initialized = true;
    }
    else
    {
        double elapsedSec = std::chrono::duration<double>(now - state.lastRefill).count();
        state.lastRefill  = now;
        double ratePerSec = static_cast<double>(ratePerMin) / 60.0;
        state.tokens = std::min(static_cast<double>(ratePerMin), state.tokens + elapsedSec * ratePerSec);
    }

    if (state.tokens < 1.0)
    {
        RecordChannelBucketAttempt(kind, /*denied=*/true);
        return false;
    }

    state.tokens -= 1.0;
    RecordChannelBucketAttempt(kind, /*denied=*/false);
    return true;
}

Channel* Hs_ResolveChannelForDelivery(Player* bot, HsChannelKind kind)
{
    ChannelMgr* cMgr = ChannelMgr::forTeam(bot->GetTeamId());
    if (!cMgr)
        return nullptr;

    if (kind == HsChannelKind::World)
        return cMgr->GetChannel("World", bot);

    uint32 chatChannelId = 0;
    bool   isCityScoped  = false; // Trade/GuildRecruitment always use AreaID 3459's "City" label
    bool   isGlobal      = false; // LookingForGroup/WorldDefense: pattern used as-is, no zone substitution
    switch (kind)
    {
        case HsChannelKind::Trade:            chatChannelId = ChatChannelId::TRADE;             isCityScoped = true; break;
        case HsChannelKind::GuildRecruitment:  chatChannelId = ChatChannelId::GUILD_RECRUITMENT;  isCityScoped = true; break;
        case HsChannelKind::General:           chatChannelId = ChatChannelId::GENERAL;                                break;
        case HsChannelKind::LocalDefense:      chatChannelId = ChatChannelId::LOCAL_DEFENSE;                          break;
        case HsChannelKind::LookingForGroup:   chatChannelId = ChatChannelId::LOOKING_FOR_GROUP;  isGlobal = true;    break;
        case HsChannelKind::WorldDefense:      chatChannelId = ChatChannelId::WORLD_DEFENSE;      isGlobal = true;    break;
        default: return nullptr; // World handled above
    }

    ChatChannelsEntry const* entry = sChatChannelsStore.LookupEntry(chatChannelId);
    if (!entry)
        return nullptr;

    uint8 locale = sWorld->GetDefaultDbcLocale();
    if (isGlobal)
        return cMgr->GetChannel(entry->pattern[locale], bot);

    AreaTableEntry const* areaEntry = isCityScoped
        ? GetAreaEntryByAreaID(3459) // "City" -- matches PlayerbotMgr.cpp's own join-time substitution
        : sAreaTableStore.LookupEntry(bot->GetZoneId());
    if (!areaEntry)
        return nullptr;

    std::string areaName = PlayerbotAI::GetLocalizedAreaName(areaEntry);
    char nameBuf[100];
    snprintf(nameBuf, sizeof(nameBuf), entry->pattern[locale], areaName.c_str());
    return cMgr->GetChannel(nameBuf, bot);
}

void Hs_CancelPendingFollowUpsFor(uint64_t senderGuid)
{
    std::lock_guard<std::mutex> lock(g_DeliveryMutex);
    g_DeliveryQueue.erase(
        std::remove_if(g_DeliveryQueue.begin(), g_DeliveryQueue.end(),
            [senderGuid](const HsPendingReply& r) { return r.isFollowUp && r.senderGuid == senderGuid; }),
        g_DeliveryQueue.end());
}

void Hs_DeliverPending()
{
    std::deque<HsPendingReply> ready;
    {
        std::lock_guard<std::mutex> lock(g_DeliveryMutex);
        if (g_DeliveryQueue.empty())
            return;

        // Almost everything has deliverAt == the tick it was queued, so this
        // is a cheap partition, not a per-tick sort. Only a self-correction
        // follow-up and a distracted reply's pair (the "sorry, was afk"
        // filler and the real reply behind it) carry a real future
        // deliverAt, and they stay in g_DeliveryQueue until their beat has
        // passed. The partition is stable, so a pair queued together still
        // drains in the order it was pushed.
        Clock::time_point now = Clock::now();
        auto notYetReady = std::stable_partition(g_DeliveryQueue.begin(), g_DeliveryQueue.end(),
            [now](const HsPendingReply& r) { return r.deliverAt <= now; });
        ready.assign(g_DeliveryQueue.begin(), notYetReady);
        g_DeliveryQueue.erase(g_DeliveryQueue.begin(), notYetReady);
        if (ready.empty())
            return;
    }

    for (auto const& reply : ready)
    {
        // A chain hop whose scope a real player took over while it was still
        // generating is dropped rather than spoken (hs_botchain.h) -- the
        // stale-line problem Hs_CancelPendingFollowUpsFor solves for
        // engagement follow-ups, checked here instead because a hop's scope
        // is a group or a channel, not the player whose message aborts it.
        // Checked ahead of the Player* lookup so an invalidated hop costs
        // nothing.
        if (reply.chainScopeId != 0 && !Hs_BotChainHopStillValid(reply.chainScopeId, reply.chainSeq))
            continue;

        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(reply.botGuid));
        if (!bot || !bot->IsInWorld())
            continue;
        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!botAI)
            continue;

        switch (reply.channel)
        {
            case HsReplyChannel::Whisper:
            {
                Player* sender = ObjectAccessor::FindPlayer(ObjectGuid(reply.senderGuid));
                if (!sender)
                    continue;
                botAI->Whisper(reply.text, sender->GetName());
                break;
            }
            case HsReplyChannel::Party: botAI->SayToParty(reply.text); break;
            case HsReplyChannel::Raid:  botAI->SayToRaid(reply.text);  break;
            case HsReplyChannel::Guild: botAI->SayToGuild(reply.text); break;
            case HsReplyChannel::Say:   botAI->Say(reply.text);        break;
            case HsReplyChannel::Channel:
            {
                Channel* channel = Hs_ResolveChannelForDelivery(bot, reply.channelKind);
                if (!channel)
                    continue; // bot no longer resolves to that channel instance (e.g. moved zones) -- drop, don't misdeliver
                channel->Say(bot->GetGUID(), reply.text, LANG_UNIVERSAL);
                break;
            }
        }

        if (g_HsDebugEnabled)
            LOG_INFO("server.loading", "[HearthsideChat] Bot {} replied: {}", bot->GetName(), reply.text);

        // Every delivered party/raid/global-channel line is a candidate seed
        // for the next hop of a bot-to-bot chain -- including a hop's own
        // line, which is how a chain gets past depth 1. Hs_NoteBotLine
        // applies every gate itself and is a cheap no-op on the surfaces and
        // tiers that don't chain. Safe to call from inside this loop:
        // g_DeliveryMutex was released before it, and the hop this may
        // enqueue lands on the *work* queue, never back on this one.
        if (reply.seedsChain)
            Hs_NoteBotLine(bot, reply.channel, reply.channelKind, reply.text, reply.chainScopeId != 0);
    }
}

uint32_t Hs_SecondsSinceLastReply(uint64_t botGuid)
{
    std::lock_guard<std::mutex> lock(g_CooldownMutex);
    auto it = g_LastReplyAt.find(botGuid);
    if (it == g_LastReplyAt.end())
        return UINT32_MAX;

    auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - it->second).count();
    return elapsedSec < 0 ? 0 : static_cast<uint32_t>(elapsedSec);
}

bool Hs_IsBackendDown()
{
    return g_BreakerOpen.load();
}

uint32_t Hs_PendingQueueDepth()
{
    std::lock_guard<std::mutex> lock(g_QueueMutex);
    return static_cast<uint32_t>(g_Queue.size());
}

bool Hs_IsReactiveIdle()
{
    if (g_ReactiveWorkerBusy.load())
        return false;
    std::lock_guard<std::mutex> lock(g_QueueMutex);
    return g_Queue.empty();
}

std::string Hs_LastPreStyleReply(const std::string& botName)
{
    std::lock_guard<std::mutex> lock(g_LastPreStyleMutex);
    auto it = g_LastPreStyleReplyByBot.find(botName);
    return it == g_LastPreStyleReplyByBot.end() ? "" : it->second;
}

const char* Hs_ReplyChannelName(HsReplyChannel channel)
{
    return ReplyChannelNameImpl(channel);
}

HsLatencyPercentiles Hs_ReactiveLatencyPercentiles()
{
    std::lock_guard<std::mutex> lock(g_MetricsMutex);
    if (g_LatencySamplesMs.empty())
        return { 0, 0, 0 };

    std::vector<uint32_t> sorted(g_LatencySamplesMs.begin(), g_LatencySamplesMs.end());
    std::sort(sorted.begin(), sorted.end());
    auto percentileAt = [&sorted](double p) {
        size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
        return sorted[idx];
    };
    return { percentileAt(0.50), percentileAt(0.95), percentileAt(0.99) };
}

HsPromptCharsByRing Hs_PromptCharsByRing()
{
    std::lock_guard<std::mutex> lock(g_MetricsMutex);
    auto mean = [](const RingPromptStats& s) {
        return s.count == 0 ? 0u : static_cast<uint32_t>(s.sumChars / s.count);
    };
    return { mean(g_PromptStatsByRing[0]), mean(g_PromptStatsByRing[1]), mean(g_PromptStatsByRing[2]) };
}

std::vector<HsArchetypeReplyCounts> Hs_ArchetypeReplyCountsSnapshot()
{
    std::lock_guard<std::mutex> lock(g_MetricsMutex);
    std::vector<HsArchetypeReplyCounts> out;
    out.reserve(g_ReplyCountsByArchetype.size());
    for (auto const& entry : g_ReplyCountsByArchetype)
        out.push_back({ entry.first, entry.second.replied, entry.second.silent });
    return out;
}

std::vector<HsChannelReplyCounts> Hs_ChannelReplyCountsSnapshot()
{
    std::lock_guard<std::mutex> lock(g_MetricsMutex);
    std::vector<HsChannelReplyCounts> out;
    out.reserve(g_ReplyCountsByChannel.size());
    for (auto const& entry : g_ReplyCountsByChannel)
        out.push_back({ static_cast<HsReplyChannel>(entry.first), entry.second.replied, entry.second.silent });
    return out;
}

HsTtlDropStats Hs_TtlDropStatsSnapshot()
{
    std::lock_guard<std::mutex> lock(g_MetricsMutex);
    return { g_TtlDroppedSession, g_TtlProcessedSession };
}

HsBucketSaturationStats Hs_GlobalBucketSaturationSnapshot()
{
    std::lock_guard<std::mutex> lock(g_MetricsMutex);
    return { g_BucketDeniedSession, g_BucketAttemptedSession };
}

std::vector<HsChannelBucketSaturationStats> Hs_ChannelBucketSaturationSnapshot()
{
    std::lock_guard<std::mutex> lock(g_MetricsMutex);
    std::vector<HsChannelBucketSaturationStats> out;
    out.reserve(g_ChannelBucketCountsByKind.size());
    for (auto const& entry : g_ChannelBucketCountsByKind)
        out.push_back({ entry.first, entry.second.replied, entry.second.silent });
    return out;
}
