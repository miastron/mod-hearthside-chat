#ifndef MOD_HS_CONFIG_H
#define MOD_HS_CONFIG_H

#include "ScriptMgr.h"
#include <cstdint>
#include <string>

// --------------------------------------------
// Core toggles
// --------------------------------------------
extern bool g_HsEnable;
extern bool g_HsDebugEnabled;

// New 2026-08-21: logs every reactive-tier (LLM) exchange -- trigger,
// pre-style reply, styled reply, archetype -- to hside_chat_log for later
// human review. A different animal from §4.7's rejected "automatic corpus
// harvesting": that was about feeding replies back into the corpus
// unreviewed; this is a raw log an operator reads and manually promotes via
// the already-built `.hearthside capture`, so it doesn't reopen that
// decision. Off by default -- new, DB-writing, and can grow large on a busy
// realm, same "operator opts in" posture as the idle-time generator.
extern bool g_HsDebugChatLogEnabled;

// --------------------------------------------
// LLM endpoint (PLAN.md §4.1) — the reactive tier only for now; the
// idle-time generator gets its own endpoint config in step 12.
// --------------------------------------------
extern std::string g_HsLLMApiType;   // llamacpp | openai | ollama
extern std::string g_HsLLMUrl;
extern std::string g_HsLLMModel;
extern std::string g_HsLLMApiKey;
extern uint32_t     g_HsLLMTimeoutSeconds;
extern uint32_t     g_HsLLMMaxTokens;
extern std::string g_HsLLMSystemPrompt;
extern uint32_t     g_HsLLMHistoryTurns;   // §4.2 — trigger/reply pairs kept per bot-player pair; 0 disables history
extern float         g_HsLLMDryMultiplier;  // §4.11 DRY retest — 0.0 leaves DRY off

// --------------------------------------------
// Reply gating — walking-skeleton subset of PLAN.md §4.8/§4.15.
// /say goes through the arbiter (hs_arbiter.h); whisper is inherently
// 1:1 and unambiguous, so it keeps a simple chance roll instead (§4.15:
// "whispering a bot is unambiguous intent"). Party/raid/guild surfaces are
// not wired up yet — step 3 only built /say and whisper.
// --------------------------------------------
extern float     g_HsSayDistance;
extern uint32_t  g_HsReplyChanceWhisper;
extern bool      g_HsDisableRepliesInCombat;

// New 2026-08-21: this module's own exclusion list, separate from
// mod-playerbots' recycling-exclusion vectors (hs_identity_store.h) --
// those protect a carded bot *from being recycled*; this one keeps a named
// bot out of Hearthside entirely (no reflex, grounded, corpus, or reactive
// reply, ever, and it never gets archetype-assigned). Name-based, matching
// mod-playerbots' own ExcludeNames convention and the same rename fragility
// it already accepts. g_HsExcludeNames is the raw comma-separated config
// value, kept for `.hearthside status`-style visibility; Hs_IsExcludedBotName
// is the fast lookup callers actually use, backed by a set parsed once in
// LoadHearthsideChatConfig rather than re-split on every chat message.
extern std::string g_HsExcludeNames;
bool Hs_IsExcludedBotName(const std::string& botName);

// --------------------------------------------
// Runtime queue (PLAN.md §4.3) — fixed worker pool of one, bounded queue,
// TTL, global token bucket, per-bot cooldown, backend-down circuit breaker.
// --------------------------------------------
extern uint32_t g_HsQueueTTLSeconds;
extern uint32_t g_HsQueueMaxDepth;
extern uint32_t g_HsBucketRepliesPerMinute;
extern uint32_t g_HsBucketBurstCapacity;
extern uint32_t g_HsBotCooldownSeconds;
extern uint32_t g_HsBreakerFailureThreshold;
extern uint32_t g_HsBreakerProbeIntervalSeconds;

// --------------------------------------------
// Typing delay (PLAN.md §3/§4.11 decision-flow's "typing delay, persona
// profile" step). Reflex/grounded/corpus-fallback replies already get a
// fixed 400-1500ms delay via Hs_DeliverReflexReply; the tier-2 (inference)
// reply had none at all -- WorkerLoop (hs_queue.cpp) delivered it the
// instant Hs_CallLLM returned, so a fast backend produced a same-tick
// reply with no human-pacing cost. Base+per-char formula matches
// mod-ollama-chat's own EnableTypingSimulation (the "reuses
// EnableTypingSimulation" PLAN.md §6 already named as the cheap way to do
// this), but computed as a residual on top of the LLM call's own real
// latency rather than an unconditional sleep, so a slow generation isn't
// double-charged.
// --------------------------------------------
extern bool     g_HsTypingDelayEnabled;
extern uint32_t g_HsTypingDelayBaseMs;
extern uint32_t g_HsTypingDelayPerCharMs;
extern uint32_t g_HsTypingDelayMaxMs;

// --------------------------------------------
// Tier ceilings (PLAN.md §4.14) — one enum, six keys, one shared parse and
// resolve helper (hs_tier.h). DirectReply and (new 2026-08-21)
// EngagementFollowUp are the only two whose consumer ever requests
// HsTier::Inference; the other four are parsed and stored but only ever
// request HsTier::Corpus, so setting them to "inference" has no additional
// effect.
// --------------------------------------------
extern std::string g_HsMaxTierDirectReply;
extern std::string g_HsMaxTierAmbient;
extern std::string g_HsMaxTierOpeners;
extern std::string g_HsMaxTierBotToBot;
extern std::string g_HsMaxTierReflex;

// New 2026-08-21 (Claude/PLAN-engagement.md): gates the engagement follow-up
// feature (hs_engagement.h). Deliberately its own key rather than reusing
// MaxTier.Openers -- Openers is checked as HsTierAllows(ceiling,
// HsTier::Corpus), hardcoded, since openers have no generated-content path;
// reusing it here would mean an operator on the documented
// MaxTier.Openers=corpus default silently caps follow-ups below inference,
// disabling the whole feature, with nothing telling them that's what
// happened. Defaults "off", not "corpus" like Openers/BotToBot -- this is
// new, GPU-doubling, autonomous, unproven behavior, same "operator opts in"
// posture as g_HsGeneratorEnabled/g_HsDebugChatLogEnabled below.
extern std::string g_HsMaxTierEngagementFollowUp;

// --------------------------------------------
// Tier-0 reflex (PLAN.md §3/§4.18/§4.20, step 9). The "are you a bot?"
// reflex is the only reflex behavior with an operator knob; the plain
// gz/ty/inv/sum/lol/wb vocabulary and the personal-probe deflection set are
// hardcoded content (hs_reflex.h), not configurable -- §3: "editing costs a
// rebuild, which is the right price for something that should not be tuned
// casually."
// --------------------------------------------
extern std::string g_HsBotQuestionMode; // wink | deflect | silent | admit

// --------------------------------------------
// Grounded answers (PLAN.md §4.20, step 10). Not a tier -- the branch sits
// beside the reflex/ceiling system (§3 decision flow), so a plain on/off
// switch is the right-sized dial rather than a sixth ordered tier value.
// The six question->template mappings themselves stay hardcoded content,
// same reasoning as §3's reflex table.
// --------------------------------------------
extern bool g_HsGroundedAnswersEnabled;

// --------------------------------------------
// Idle-time generator (PLAN.md §4.7, step 12). Its own LLM endpoint, kept
// separate from the reactive path's (§4.1 decision) -- "whether or not they
// currently point at the same container." Defaults to disabled: this is new,
// autonomous, GPU-spending, DB-writing background work, so an operator opts
// in rather than it starting the moment the module is enabled.
// --------------------------------------------
extern bool        g_HsGeneratorEnabled;
extern std::string g_HsGeneratorLLMApiType;
extern std::string g_HsGeneratorLLMUrl;
extern std::string g_HsGeneratorLLMModel;
extern std::string g_HsGeneratorLLMApiKey;
extern uint32_t     g_HsGeneratorLLMTimeoutSeconds;
extern uint32_t     g_HsGeneratorLLMMaxTokens;
extern uint32_t     g_HsGeneratorRowsPerBucket;          // §4.5: per-bucket quota, never global
extern uint32_t     g_HsGeneratorPollIntervalSeconds;     // recheck cadence while reactive is busy
extern uint32_t     g_HsGeneratorQuotaSatisfiedBackoffSeconds; // backoff once nothing is under quota
extern std::string g_HsGeneratorPromptVersion;            // tags generated rows for bulk-evict (§4.4)

// §4.7's third work queue / §7 step 14: the reserve target for scripted
// bot-to-bot conversations (hs_script.h). A producer-feeding-a-consumer
// target, not a per-bucket quota (§4.7: "targeting a reserve depth ... not
// a bucket-filler that throttles to near-zero once quota is met"). Takes
// priority over bucket-filling on every generator cycle while under target,
// same "cards, then script reserve, then buckets" order §4.7 names (cards
// don't exist until step 15).
extern uint32_t     g_HsGeneratorScriptReserveTarget;

// --------------------------------------------
// Observability and the control API (PLAN.md §4.19, step 19). An
// authenticated HTTP server lifted from mod-playerbots-characters' own
// (Unlicense) implementation -- same config-key shape (Port/Bind/
// PrivateKey/TimeoutSeconds), minus PBC's per-account OTP/session-token
// system and its FrontendPath static-file serving, neither of which this
// single-operator admin tool has a use for (named gaps, not oversights --
// see PROGRESS.md). Bearer-token auth is a direct equality check against
// PrivateKey, not PBC's AES-encrypted per-account tokens -- this module has
// no concept of multiple authenticated web users to distinguish between.
// Read routes need only the token; mutating routes need the token *and*
// HttpControlEnable, PLAN.md's own "different risk surfaces" split.
// --------------------------------------------
extern uint32_t     g_HsHttpServerPort;           // 0 = disabled
extern std::string g_HsHttpServerBind;            // loopback by default
extern std::string g_HsHttpServerPrivateKey;      // required when port != 0
extern uint32_t     g_HsHttpServerTimeoutSeconds;
extern bool         g_HsHttpControlEnable;

void LoadHearthsideChatConfig();

class HsConfigWorldScript : public WorldScript
{
public:
    HsConfigWorldScript();
    void OnStartup() override;
};

#endif // MOD_HS_CONFIG_H
