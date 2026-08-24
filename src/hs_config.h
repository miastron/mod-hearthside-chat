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

// Gates HsBridgePlayerScript (hs_bridge.h) -- the read-only HSI addon-
// message bridge the HearthsideInspect client addon talks to. Independent
// of g_HsEnable: a realm could run the bridge for the Inspect-tab feature
// while the chat/reply pipeline itself is off, or vice versa.
extern bool g_HsBridgeEnable;

// Logs every reactive-tier (LLM) exchange -- trigger, pre-style reply,
// styled reply, archetype -- to hside_chat_log for later human review. This
// is a raw log an operator reads and manually promotes via `.hearthside
// capture`; it does not feed replies back into the corpus automatically.
// Off by default -- DB-writing and can grow large on a busy realm.
extern bool g_HsDebugChatLogEnabled;

// --------------------------------------------
// LLM endpoint -- the reactive tier's. The idle-time generator uses its own,
// separate endpoint config (below).
// --------------------------------------------
extern std::string g_HsLLMApiType;   // llamacpp | openai | ollama
extern std::string g_HsLLMUrl;
extern std::string g_HsLLMModel;
extern std::string g_HsLLMApiKey;
extern uint32_t     g_HsLLMTimeoutSeconds;
extern uint32_t     g_HsLLMMaxTokens;
extern std::string g_HsLLMTemplate;  // llama3 | chatml -- only meaningful when ApiType=llamacpp (see hs_llm.h's HsLLMConfig::templateKind)
extern std::string g_HsLLMSystemPrompt;
extern uint32_t     g_HsLLMHistoryTurns;   // trigger/reply pairs kept per bot-player pair; 0 disables history
extern float         g_HsLLMDryMultiplier;  // 0.0 leaves DRY off

// --------------------------------------------
// Reply gating. /say goes through the arbiter (hs_arbiter.h); whisper is
// inherently 1:1 and unambiguous, so it keeps a simple chance roll instead.
// Party/raid/guild surfaces are not wired up yet -- only /say and whisper.
// --------------------------------------------
extern float     g_HsSayDistance;
extern uint32_t  g_HsReplyChanceWhisper;
extern bool      g_HsDisableRepliesInCombat;

// This module's own exclusion list, separate from mod-playerbots' recycling-
// exclusion vectors (hs_identity_store.h) -- those protect a carded bot from
// being recycled; this one keeps a named bot out of Hearthside entirely (no
// reflex, grounded, corpus, or reactive reply, ever, and no archetype
// assignment). Name-based, matching mod-playerbots' own ExcludeNames
// convention. g_HsExcludeNames is the raw comma-separated config value, kept
// for `.hearthside status`-style visibility; Hs_IsExcludedBotName is the fast
// lookup callers use, backed by a set parsed once in LoadHearthsideChatConfig
// rather than re-split on every chat message.
extern std::string g_HsExcludeNames;
bool Hs_IsExcludedBotName(const std::string& botName);

// --------------------------------------------
// Runtime queue -- fixed worker pool of one, bounded queue, TTL, global
// token bucket, per-bot cooldown, backend-down circuit breaker.
// --------------------------------------------
extern uint32_t g_HsQueueTTLSeconds;
extern uint32_t g_HsQueueMaxDepth;
extern uint32_t g_HsBucketRepliesPerMinute;
extern uint32_t g_HsBucketBurstCapacity;
extern uint32_t g_HsBotCooldownSeconds;
extern uint32_t g_HsBreakerFailureThreshold;
extern uint32_t g_HsBreakerProbeIntervalSeconds;

// --------------------------------------------
// Typing delay for the tier-2 (inference) reply. Reflex/grounded/corpus-
// fallback replies already get a fixed 400-1500ms delay via
// Hs_DeliverReflexReply, and scripted turns (hs_script.cpp) their own
// 800-2000ms first-turn delay plus 4-7s inter-turn gaps -- both already
// floored. The base+per-char formula is per-archetype
// (hs_archetype.h's typingBaseMs/typingPerCharMs, hside_archetype SQL);
// these two keys are just the kill switch and ceiling, same relationship
// LLM.MaxTokens has with verbosityCap. Computed as a residual on top of the
// LLM call's own real latency, so a slow generation isn't double-charged.
//
// MinDeliveryDelayMs is a separate, unconditional floor on that same
// residual: TypingDelay.Enable=false leaves tier-2 with nothing bounding how
// early a reply can land (TTL only bounds how late), so a fast backend could
// otherwise deliver same-tick. Applies regardless of the Enable toggle
// (Claude/ISSUES.md's "minimum delivery delay" open question).
// --------------------------------------------
extern bool     g_HsTypingDelayEnabled;
extern uint32_t g_HsTypingDelayMaxMs;
extern uint32_t g_HsMinDeliveryDelayMs;

// --------------------------------------------
// Tier ceilings -- one enum, six keys, one shared parse and resolve helper
// (hs_tier.h). DirectReply and EngagementFollowUp are the only two whose
// consumer ever requests HsTier::Inference; the other four are parsed and
// stored but only ever request HsTier::Corpus, so setting them to
// "inference" has no additional effect.
// --------------------------------------------
extern std::string g_HsMaxTierDirectReply;
extern std::string g_HsMaxTierAmbient;
extern std::string g_HsMaxTierOpeners;
extern std::string g_HsMaxTierBotToBot;
extern std::string g_HsMaxTierReflex;

// Gates the engagement follow-up feature (hs_engagement.h). Deliberately its
// own key rather than reusing MaxTier.Openers -- Openers is checked as
// HsTierAllows(ceiling, HsTier::Corpus), hardcoded, since openers have no
// generated-content path; reusing it here would silently cap follow-ups
// below inference on the documented MaxTier.Openers=corpus default, with
// nothing telling the operator that's what happened. Defaults "off", not
// "corpus" like Openers/BotToBot -- autonomous, GPU-doubling behavior that
// should be an explicit opt-in.
extern std::string g_HsMaxTierEngagementFollowUp;

// --------------------------------------------
// §4.17 global-channel chat surface -- one MaxTier/RatePerMin/MaxCandidates
// triple per channel, same "off | reflex | corpus | inference" enum as the
// MaxTier.* family above, capped at Corpus in practice the same way Openers
// already is (hs_channel.cpp has no generated-content path). No existing
// precedent in this module for a parameterized key family, so these are 21
// explicit keys rather than a loop-driven one, matching how every other
// HearthsideChat.* key is declared. Parsed here, then folded into an
// HsChannelPolicy table (hs_channel.h) by LoadHearthsideChatConfig so the
// hot path (hs_handler.cpp's Channel* hook) never touches config strings
// directly.
// --------------------------------------------
extern std::string g_HsChannelTradeMaxTier;
extern uint32_t     g_HsChannelTradeRatePerMin;
extern uint32_t     g_HsChannelTradeMaxCandidates;

extern std::string g_HsChannelGeneralMaxTier;
extern uint32_t     g_HsChannelGeneralRatePerMin;
extern uint32_t     g_HsChannelGeneralMaxCandidates;

extern std::string g_HsChannelWorldMaxTier;
extern uint32_t     g_HsChannelWorldRatePerMin;
extern uint32_t     g_HsChannelWorldMaxCandidates;

extern std::string g_HsChannelLookingForGroupMaxTier;
extern uint32_t     g_HsChannelLookingForGroupRatePerMin;
extern uint32_t     g_HsChannelLookingForGroupMaxCandidates;

extern std::string g_HsChannelGuildRecruitmentMaxTier;
extern uint32_t     g_HsChannelGuildRecruitmentRatePerMin;
extern uint32_t     g_HsChannelGuildRecruitmentMaxCandidates;

extern std::string g_HsChannelLocalDefenseMaxTier;
extern uint32_t     g_HsChannelLocalDefenseRatePerMin;
extern uint32_t     g_HsChannelLocalDefenseMaxCandidates;

extern std::string g_HsChannelWorldDefenseMaxTier;
extern uint32_t     g_HsChannelWorldDefenseRatePerMin;
extern uint32_t     g_HsChannelWorldDefenseMaxCandidates;

// --------------------------------------------
// Tier-0 reflex. The "are you a bot?" reflex is the only reflex behavior
// with an operator knob; the plain gz/ty/inv/sum/lol/wb vocabulary and the
// personal-probe deflection set are hardcoded content (hs_reflex.h), not
// configurable -- editing costs a rebuild, the right price for something
// that shouldn't be tuned casually.
// --------------------------------------------
extern std::string g_HsBotQuestionMode; // wink | deflect | silent | admit

// --------------------------------------------
// Grounded answers. Not a tier -- the branch sits beside the reflex/ceiling
// system, so a plain on/off switch is the right-sized dial rather than a
// sixth ordered tier value. The question/template content itself lives in
// hside_grounded_question/hside_grounded_template (hs_grounded_store.cpp),
// not here -- these two keys are the matcher's own tuning, not content.
// --------------------------------------------
extern bool     g_HsGroundedAnswersEnabled;
extern uint32_t g_HsGroundedFuzzyMaxDistance; // max Levenshtein distance for the typo-tolerance fallback pass; 0 disables it (exact match only)

// --------------------------------------------
// Idle-time generator. Its own LLM endpoint, kept separate from the
// reactive path's, regardless of whether they point at the same container.
// Defaults to disabled: autonomous, GPU-spending, DB-writing background
// work, so an operator opts in rather than it starting the moment the
// module is enabled.
// --------------------------------------------
extern bool        g_HsGeneratorEnabled;
extern std::string g_HsGeneratorLLMApiType;
extern std::string g_HsGeneratorLLMUrl;
extern std::string g_HsGeneratorLLMModel;
extern std::string g_HsGeneratorLLMApiKey;
extern uint32_t     g_HsGeneratorLLMTimeoutSeconds;
extern uint32_t     g_HsGeneratorLLMMaxTokens;
extern std::string g_HsGeneratorLLMTemplate; // llama3 | chatml -- same as g_HsLLMTemplate, generator's own endpoint
extern uint32_t     g_HsGeneratorRowsPerBucket;          // per-bucket quota, never global
extern uint32_t     g_HsGeneratorPollIntervalSeconds;     // recheck cadence while reactive is busy
extern uint32_t     g_HsGeneratorQuotaSatisfiedBackoffSeconds; // backoff once nothing is under quota
extern std::string g_HsGeneratorPromptVersion;            // tags generated rows for bulk-evict

// The reserve target for scripted bot-to-bot conversations (hs_script.h). A
// producer-feeding-a-consumer target, not a per-bucket quota -- it takes
// priority over bucket-filling on every generator cycle while under target,
// following the same "cards, then script reserve, then buckets" order the
// generator uses.
extern uint32_t     g_HsGeneratorScriptReserveTarget;

// --------------------------------------------
// Observability and the control API. An authenticated HTTP server lifted
// from mod-playerbots-characters' own (Unlicense) implementation -- same
// config-key shape (Port/Bind/PrivateKey/TimeoutSeconds), minus PBC's per-
// account OTP/session-token system and its FrontendPath static-file
// serving, neither of which this single-operator admin tool needs.
// Bearer-token auth is a direct equality check against PrivateKey, not
// PBC's AES-encrypted per-account tokens -- this module has no concept of
// multiple authenticated web users to distinguish between. Read routes need
// only the token; mutating routes need the token *and* HttpControlEnable.
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

    // `.reload config` re-reads every HearthsideChat.* key. Gated on
    // `reload` since OnStartup already covers the initial load.
    void OnAfterConfigLoad(bool reload) override;
};

#endif // MOD_HS_CONFIG_H
