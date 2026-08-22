#include "hs_queue.h"
#include "hs_archetype.h"
#include "hs_config.h"
#include "hs_engagement.h"
#include "hs_identity.h"
#include "hs_identity_store.h"
#include "hs_llm.h"
#include "hs_memory_store.h"
#include "hs_style.h"

#include "DatabaseEnv.h" // new 2026-08-21: HearthsideChat.DebugChatLog insert
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "ObjectAccessor.h"
#include "Random.h"

#include <algorithm>
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

    // §4.11's self-correction follow-up: "~5% (decided)" is a settled design
    // constant, not an operator knob, so it lives here rather than in
    // hs_config.h. 2-5s for "a beat later" is a starting guess — there's no
    // typing-delay profile in this module yet to derive it from.
    constexpr uint32_t kSelfCorrectionChancePercent   = 5;
    constexpr uint32_t kSelfCorrectionMinDelaySeconds = 2;
    constexpr uint32_t kSelfCorrectionMaxDelaySeconds = 5;

    // New 2026-08-21: a short, factual line describing what the bot is
    // actually doing right now (mod-playerbots' own NewRpgStatus), appended
    // to personaLine in WorkerLoop below so a free-generating reply can't
    // contradict observable state -- "pvping" while the bot is mid-quest is
    // exactly the kind of tell §1's retreat rule exists to prevent. Plain
    // statement of fact, not a behavioural instruction -- the same register
    // §4.20's grounded answers and the card voice block already use for
    // things the model is simply told rather than asked to obey. RPG_IDLE
    // and the exploratory wander states return empty -- "idle" or
    // "wandering" isn't a distinctive enough claim to be worth the tokens or
    // the risk of sounding stilted ("I'm currently idle").
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

    // §3 tier 0: "real players fire these back in under a second" -- a
    // same-tick reply would be more of a tell than the LLM's latency this
    // tier exists to avoid, so it still gets a short, randomized delay.
    // Settled design constant, not an operator knob, same as the
    // self-correction delay above.
    constexpr uint32_t kReflexDelayMinMs = 400;
    constexpr uint32_t kReflexDelayMaxMs = 1500;

    struct HsQueuedRequest
    {
        uint64_t     botGuid;
        std::string  botName;     // §4.11 style pass: protected from typo injection
        uint64_t     senderGuid;
        std::string  senderName;  // §4.11 style pass: protected from typo injection
        bool         isWhisper;
        std::string  prompt;
        Clock::time_point enqueuedAt;
        bool         isProbe;
        bool         inCombat;    // §4.11 style pass: combat `care` offset
        uint8_t      botLevel;    // §4.13 archetype eligibility filter (hs_archetype.h)
        NewRpgStatus rpgStatus;   // new 2026-08-21: live activity fact, folded into personaLine below
        bool         isFollowUp;  // new 2026-08-21: self-initiated engagement follow-up (hs_engagement.h) -- no score, no history write
    };

    // deliverAt lets one worker-thread turn queue two chat lines instead of
    // one: the reply after its typing delay, and (§4.11's self-correction
    // follow-up) a `*correction` a beat after that. Reflex/grounded/corpus-
    // fallback replies (delivered via Hs_DeliverReflexReply, not this
    // struct's other producer below) still use their own fixed 400-1500ms
    // window regardless of text length.
    struct HsPendingReply
    {
        uint64_t    botGuid;
        uint64_t    senderGuid;
        bool        isWhisper;
        std::string text;
        Clock::time_point deliverAt;
        bool        isFollowUp; // new 2026-08-21: cancellable via Hs_CancelPendingFollowUpsFor; direct replies and the self-correction addendum are never tagged
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

    // ---- token bucket. §4.3: the primary load ceiling. ----
    std::mutex          g_BucketMutex;
    double               g_BucketTokens = 0.0;
    Clock::time_point    g_BucketLastRefill;
    bool                 g_BucketInitialized = false;

    // ---- per-bot cooldown (gate) and last-successful-reply time (arbiter query) ----
    std::mutex                                        g_CooldownMutex;
    std::unordered_map<uint64_t, Clock::time_point>   g_LastEnqueueAt;
    std::unordered_map<uint64_t, Clock::time_point>   g_LastReplyAt;

    // ---- conversation history (§4.2): four lines per bot-player pair, held
    // in memory only. Keyed by (botGuid, senderGuid); std::map's operator<
    // on std::pair needs no custom hash. ----
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

    // §4.2: "a turn's trigger becomes the next turn's first history line
    // byte-for-byte" — store the exact trigger/reply just used, then evict
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

    // ---- §4.7 idle signal for the generator: true only while the worker
    // thread is actually inside Hs_CallLLM. ----
    std::atomic<bool> g_ReactiveWorkerBusy{false};

    // ---- §4.7 `.hearthside capture`: last pre-style reply per bot name.
    // Overwritten on every successful reactive reply -- "the last one," not
    // a history. ----
    std::mutex                                  g_LastPreStyleMutex;
    std::unordered_map<std::string, std::string> g_LastPreStyleReplyByBot;

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
    // as needed. Edge-triggered logging only — one line per transition, never
    // per request (§4.3: "silent" while open, not a log line per rejection).
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
                if (req.isProbe)
                    g_ProbeInFlight.store(false);
                if (g_HsDebugEnabled)
                    LOG_INFO("server.loading", "[HearthsideChat] Dropping stale request for bot {} (age {}s > TTL {}s).",
                        req.botGuid, ageSec, g_HsQueueTTLSeconds);
                continue;
            }

            // §4.11 step 7 / §4.13 step 8: the bot's archetype, drawn
            // deterministically from its GUID and restricted to the
            // level-eligible pool (hs_archetype.h). Feeds both the LLM
            // prompt's delta layer and the style pass's `care` baseline
            // below.
            HsArchetype archetype = Hs_ArchetypeForBot(req.botGuid, req.botLevel);
            const HsArchetypeInfo& archetypeInfo = Hs_ArchetypeInfoFor(archetype);

            // §4.12: the voice block is "the only card text that ever
            // enters a prompt" -- appended after the archetype delta line
            // and before history, same layer ordering PLAN.md specifies.
            // Folded into the same archetypeLine parameter Hs_CallLLM
            // already takes (rather than widening that function's
            // signature) since the two are byte-adjacent in the assembled
            // prompt either way; a no-op string concat for the ring 0-2
            // majority with no active card.
            HsCardSnapshot cardSnapshot = Hs_LookupCardSnapshot(req.botGuid);
            std::string personaLine = Hs_ArchetypePromptLine(archetype);
            if (cardSnapshot.active && !cardSnapshot.voiceBlock.empty())
                personaLine += "\n" + cardSnapshot.voiceBlock;
            std::string rpgHint = RpgStatusHint(req.rpgStatus);
            if (!rpgHint.empty())
                personaLine += "\n" + rpgHint;

            HsLLMConfig cfg;
            cfg.apiType       = g_HsLLMApiType;
            cfg.baseUrl       = g_HsLLMUrl;
            cfg.model         = g_HsLLMModel;
            cfg.apiKey        = g_HsLLMApiKey;
            cfg.timeoutSec    = static_cast<int>(g_HsLLMTimeoutSeconds);
            // Per-archetype verbosity cap (§4.11 "terse archetypes get a
            // 25-token cap, not 60") refines the operator's configured
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

            if (!result.success || result.text.empty())
            {
                if (g_HsDebugEnabled)
                    LOG_INFO("server.loading", "[HearthsideChat] No reply for bot {} (failure={}, httpStatus={}).",
                        req.botGuid, static_cast<int>(result.failure), result.httpStatus);
                continue; // silence, not a canned fallback — §1 retreat rule
            }

            // §4.7 `.hearthside capture`: stash the *pre-style* text before
            // the style pass below reshapes it -- "the honest version of the
            // idea," so what a GM captures is the model's clean output, not
            // this specific reply's typos (trap 12).
            {
                std::lock_guard<std::mutex> lock(g_LastPreStyleMutex);
                g_LastPreStyleReplyByBot[req.botName] = result.text;
            }

            // §4.11 style post-processor (step 6): caps/punctuation/abbrev
            // reshaping and typo injection, applied before the text becomes
            // either delivered chat or the next turn's history line — so a
            // re-rendered history line and the one actually spoken always
            // match byte-for-byte (§4.2's append-chain requirement). `care`'s
            // baseline is the archetype's (step 7); TRADER is the only entry
            // with an abbreviation override today.
            HsStyleContext styleCtx;
            styleCtx.baselineCare         = archetypeInfo.care;
            styleCtx.abbrevOverrideChance = archetypeInfo.hasAbbrevOverride ? archetypeInfo.abbrevOverrideChance : -1.0f;
            styleCtx.inCombat             = req.inCombat;
            styleCtx.verbalTic            = cardSnapshot.verbalTic;
            // §7 new 2026-08-21: capture both the pre-style and post-style
            // text for HearthsideChat.DebugChatLog before result.text is
            // overwritten below -- one row per reactive-tier exchange, for
            // an operator to review later and hand-pick examples that need
            // improvement or belong in a training set. Off the critical
            // path either way (CharacterDatabase.Execute is fire-and-forget,
            // and this thread already calls it elsewhere -- Hs_BumpInteraction
            // Score below does the same).
            std::string preStyleForLog = result.text;

            HsStyleResult style = Hs_ApplyStyle(req.botGuid, req.botName, req.senderName, result.text, styleCtx);
            result.text = style.text;
            if (result.text.empty())
                continue;

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
                    req.botGuid, escapedBotName, req.senderGuid, escapedSenderName, req.isWhisper ? 1 : 0,
                    escapedArchetype, escapedTrigger, escapedPreStyle, escapedStyled);
            }

            // History stores only the primary reply, not the follow-up
            // correction below — a bare "*healer" fragment isn't useful
            // prior-turn context for the LLM, and the corrected meaning is
            // already fully present in result.text (the typo is spelling
            // noise, not a different word). An engagement follow-up
            // (hs_engagement.h) gets the same non-write treatment and for
            // the same reason -- its own "trigger" is a synthetic
            // instruction, not something the player actually said, so
            // replaying it as a prior turn would be confusing rather than
            // useful context.
            if (!req.isFollowUp)
                HistoryAppend(req.botGuid, req.senderGuid, req.prompt, result.text);

            // §4.12 "count conversation, not groups": scores for exactly the
            // bot the arbiter selected, exactly when the reply resolves to
            // tier 2 -- this is the only point in the module where that's
            // true. Reflex/grounded/corpus-fallback replies never reach
            // here (§4.12/§4.15: "tier 0 stays completely free of identity
            // side effects", and the corpus/grounded paths inherit that
            // same "answer without the GPU" shape). An engagement follow-up
            // is bot-initiated, not a scored player utterance -- same rule
            // openers already follow (§4.12: "bot-initiated openers must
            // not increment interaction_score").
            if (!req.isFollowUp)
            {
                Hs_BumpInteractionScore(req.botGuid, req.botLevel,
                    req.isWhisper ? kHsScoreWeightWhisper : kHsScoreWeightSay);
            }

            // New 2026-08-21: re-arms this (bot, player) pair's engagement-
            // follow-up eligibility -- only a genuine direct reply does this,
            // never a follow-up's own delivery (Claude/PLAN-engagement.md:
            // a chain only continues as long as the player keeps replying).
            if (!req.isFollowUp)
                Hs_EngagementNoteDirectReply(req.botGuid, req.senderGuid, req.isWhisper);

            // §4.12 step 16: ordinary chat is by far the most common way two
            // people actually meet, so first-meeting is seeded here rather
            // than only as a side effect of the rarer shared-experience
            // events (dungeon/group/death/guild). Idempotent -- a no-op on
            // every exchange after the pair's first.
            Hs_EnsureFirstMeetingRecorded(req.botGuid, req.senderGuid);

            {
                std::lock_guard<std::mutex> lock(g_CooldownMutex);
                g_LastReplyAt[req.botGuid] = Clock::now();
            }

            Clock::time_point now = Clock::now();

            // New 2026-08-21: typing delay for the tier-2 reply (PLAN.md
            // §3/§4.11 decision flow's "typing delay, persona profile"
            // step -- previously unbuilt for this path, see PROGRESS.md's
            // known-gaps list). A residual on top of however long
            // Hs_CallLLM already took, so the total (real generation
            // latency + top-up) approximates a human typing the reply,
            // without ever shortening what the LLM call itself already
            // cost. A fast backend that would otherwise deliver same-tick
            // still gets the full target delay.
            Clock::time_point deliverAt = now;
            if (g_HsTypingDelayEnabled)
            {
                uint32_t targetMs = std::min(g_HsTypingDelayMaxMs,
                    g_HsTypingDelayBaseMs + static_cast<uint32_t>(result.text.size()) * g_HsTypingDelayPerCharMs);
                int64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - req.enqueuedAt).count();
                if (elapsedMs < static_cast<int64_t>(targetMs))
                    deliverAt = now + std::chrono::milliseconds(static_cast<uint32_t>(targetMs) - static_cast<uint32_t>(elapsedMs));
            }

            {
                std::lock_guard<std::mutex> lock(g_DeliveryMutex);
                g_DeliveryQueue.push_back({ req.botGuid, req.senderGuid, req.isWhisper, result.text, deliverAt, req.isFollowUp });

                // §4.11 self-correction follow-up, ~5% (decided): only
                // eligible when a typo actually landed in this message. The
                // `*` prefix is added here, after the style pass, and the
                // corrected word is exempt from it — a plain literal fix,
                // not another sloppy line. "A beat later" gets a short
                // random delay measured from the primary reply's own
                // deliverAt (not `now`) so the correction can never arrive
                // before the line it corrects.
                if (!style.correction.empty() && urand(0, 99) < kSelfCorrectionChancePercent)
                {
                    uint32_t delaySec = urand(kSelfCorrectionMinDelaySeconds, kSelfCorrectionMaxDelaySeconds);
                    std::string followUp = "*" + style.correction;
                    g_DeliveryQueue.push_back({ req.botGuid, req.senderGuid, req.isWhisper, followUp,
                                                 deliverAt + std::chrono::seconds(delaySec), req.isFollowUp });
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
                    const std::string& senderName, bool isWhisper, const std::string& userPrompt,
                    bool inCombat, uint8_t botLevel, NewRpgStatus rpgStatus, bool isFollowUp)
{
    // 1. Token bucket — peek only; the spend is committed once the request
    // actually clears every later gate.
    {
        std::lock_guard<std::mutex> lock(g_BucketMutex);
        RefillBucketLocked();
        if (g_BucketTokens < 1.0)
            return false;
    }

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
        req.isWhisper  = isWhisper;
        req.prompt     = userPrompt;
        req.enqueuedAt = Clock::now();
        req.isProbe    = isProbe;
        req.inCombat   = inCombat;
        req.botLevel   = botLevel;
        req.rpgStatus  = rpgStatus;
        req.isFollowUp = isFollowUp;
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

void Hs_DeliverReflexReply(uint64_t botGuid, uint64_t senderGuid, bool isWhisper, const std::string& text)
{
    if (text.empty())
        return;

    Clock::time_point deliverAt = Clock::now() + std::chrono::milliseconds(urand(kReflexDelayMinMs, kReflexDelayMaxMs));
    std::lock_guard<std::mutex> lock(g_DeliveryMutex);
    g_DeliveryQueue.push_back({ botGuid, senderGuid, isWhisper, text, deliverAt, /*isFollowUp=*/false });
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

        // Almost everything has deliverAt == the tick it was queued, so
        // this is a cheap partition, not a per-tick sort. Only §4.11's
        // self-correction follow-up carries a real future deliverAt, and it
        // stays in g_DeliveryQueue until its beat has passed.
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
        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(reply.botGuid));
        if (!bot || !bot->IsInWorld())
            continue;
        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!botAI)
            continue;

        if (reply.isWhisper)
        {
            Player* sender = ObjectAccessor::FindPlayer(ObjectGuid(reply.senderGuid));
            if (!sender)
                continue;
            botAI->Whisper(reply.text, sender->GetName());
        }
        else
        {
            botAI->Say(reply.text);
        }

        if (g_HsDebugEnabled)
            LOG_INFO("server.loading", "[HearthsideChat] Bot {} replied: {}", bot->GetName(), reply.text);
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
