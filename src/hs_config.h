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
// Reading the std::string globals from a thread that is not the world thread
//
// `.reload config` re-runs LoadHearthsideChatConfig on the world thread,
// which reassigns every std::string global below wholesale. Three other
// threads read them concurrently and can hold one for a long time:
//
//   - the queue worker (hs_queue.cpp), which passes g_HsLLMSystemPrompt to
//     Hs_CallLLM by const& and keeps that reference live for the whole HTTP
//     round trip -- up to LLM.TimeoutSeconds;
//   - the generator (hs_generator.cpp), same shape on its own six keys;
//   - the HTTP server (hs_http_auth.cpp), which indexes
//     g_HsHttpServerPrivateKey character by character inside the
//     constant-time token compare.
//
// Reassigning a std::string frees its buffer when the new value does not fit
// the old allocation, so an unguarded read is a use-after-free, not a stale
// value. (The default system prompt is ~220 bytes -- far past SSO -- so any
// operator edit to it is a heap reallocation.) This is the same hazard, and
// the same fix, that hs_archetype.cpp:196-203 documents for g_Archetypes,
// whose HsArchetypeInfo likewise owns a std::string.
//
// The scalars are deliberately left bare: a torn uint32_t/float/bool is
// formally UB but benign on x86-64, and costs one request a wrong number
// rather than a freed buffer. Only the strings are a memory-safety problem,
// which is what keeps this small.
//
// Two batched snapshots for the hot paths (one lock per request instead of
// six), and one generic accessor for everything else -- Hs_ConfigString takes
// the global by reference but copies it *inside* the lock, so the reference
// never outlives the writer's exclusion.
// --------------------------------------------
struct HsLLMStrings
{
    std::string apiType;
    std::string url;
    std::string model;
    std::string apiKey;
    std::string templateKind;
    std::string systemPrompt;
};

struct HsGeneratorStrings
{
    std::string apiType;
    std::string url;
    std::string model;
    std::string apiKey;
    std::string templateKind;
    std::string promptVersion;
};

HsLLMStrings       Hs_LLMStringsSnapshot();
HsGeneratorStrings Hs_GeneratorStringsSnapshot();

// Copy one string global under the config lock. Call as
// Hs_ConfigString(g_HsMaxTierAmbient). World-thread callers do not need it
// (they are the writer), but it is harmless there.
std::string Hs_ConfigString(const std::string& configGlobal);

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
// Reply gating. /say, party/raid (subgroup-scoped for CHAT_MSG_PARTY), guild
// and the §4.17 global channels all go through the arbiter (hs_arbiter.h) --
// see the four candidate scans in hs_handler.cpp. Whisper is the one surface
// that does not: it is inherently 1:1 and unambiguous, so it keeps a simple
// chance roll instead.
// --------------------------------------------
extern float     g_HsSayDistance;
extern uint32_t  g_HsReplyChanceWhisper;
extern bool      g_HsDisableRepliesInCombat;

// How many bots answer a single message -- /say, party/raid, guild, channel,
// or a live bot-to-bot chain hop -- once the arbiter has an eligible
// candidate pool (hs_arbiter.cpp's PickReplyCount). Whisper is 1:1 and skips
// this entirely (ReplyChance.Whisper above). Weights, not cumulative
// percentages -- each is the relative share of the roll that lands on that
// count, summed and normalized at load time, so they need not add to 100 and
// there is no ordering constraint between them. Defaults are 30/60/10,
// weighted more toward a single reply than the module's original hardcoded
// 50%/42%/8% split.
extern uint32_t  g_HsReplyCountZeroPercent;
extern uint32_t  g_HsReplyCountOnePercent;
extern uint32_t  g_HsReplyCountTwoPercent;

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
// Distracted reply -- the "sorry, was afk" flavor. Rolled per completed
// tier-2 reply against the bot archetype's own distracted_chance
// (hside_archetype SQL); on a hit the bot sends a canned filler line after
// MinDelaySeconds..MaxDelaySeconds, then the real reply a full typing delay
// after that. Same kill-switch-plus-per-archetype-formula relationship
// TypingDelay.Enable has with typing_base_ms.
//
// Deliberately not tied to real backend latency or Queue.TTLSeconds: a TTL
// expiry means the request was dropped before inference, not delivered late,
// and on an unstressed GPU a latency-triggered version would never fire at
// all. This is characterization, not backpressure.
//
// MinDelaySeconds is the load-bearing knob: "sorry, was afk" five seconds
// later is a transparent lie, so the floor has to be long enough to justify
// the apology while staying short of reading as a broken bot. CooldownSeconds
// is the anti-frustration bound -- one bot cannot pull this on a player again
// until it elapses, however high its archetype's chance is.
// --------------------------------------------
extern bool     g_HsDistractedEnabled;
extern uint32_t g_HsDistractedMinDelaySeconds;
extern uint32_t g_HsDistractedMaxDelaySeconds;
extern uint32_t g_HsDistractedCooldownSeconds;

// --------------------------------------------
// Tier ceilings -- one enum, seven keys, one shared parse and resolve helper
// (hs_tier.h). DirectReply, EngagementFollowUp, Events and BotToBot are the
// four whose consumer ever requests HsTier::Inference. Openers and Reflex are
// gated but only ever at HsTier::Corpus and HsTier::Reflex respectively, so
// setting either to "inference" has no additional effect.
//
// Ambient (hs_ambient.h, Claude/PLAN-AMBIENT.md) is unprompted chatter with
// no trigger at all -- a bot speaks because there is dead air near a real
// player, not because anything happened. Checked as
// HsTierAllows(ceiling, HsTier::Corpus), same as Openers -- corpus-only,
// deliberately: unprompted GPU spend against no question is the worst
// cost-per-value in the module.
//
// BotToBot is the one key with two consumers at different tiers, and it works
// because a ceiling is permissive rather than a mode switch: at "corpus"
// hs_script.cpp replays pre-generated scripts and nothing else; at
// "inference" hs_botchain.h's live chains run *in addition to* that replay,
// which still passes its own HsTier::Corpus check. Script pre-generation is
// unaffected by this key either way -- hs_generator.cpp gates the reserve on
// Generator.Enable alone, so the GPU keeps filling it during idle at any
// BotToBot setting.
// --------------------------------------------
extern std::string g_HsMaxTierDirectReply;
extern std::string g_HsMaxTierAmbient;
extern std::string g_HsMaxTierOpeners;
extern std::string g_HsMaxTierBotToBot;
extern std::string g_HsMaxTierReflex;

// --------------------------------------------
// Ambient (hs_ambient.h). Ambient has no natural rate limiter -- every other
// unprompted surface is bounded by how often its trigger fires (a player
// speaks, a game event happens); ambient is bounded only by the clock. So
// its bucket below is deliberately shared with the other two unprompted-
// speech producers, hs_opener.cpp's FireOpener and hs_script.cpp's scene
// claims (Hs_AmbientBucketTake, hs_queue.h) -- three independent producers
// each individually tuned to "reasonable" could otherwise still stack into
// constant noise. Sized well under Events.Bucket.RepliesPerMinute: ambient's
// failure mode (a realm that reads as a bot farm) is worse than the event
// surface's, and a too-noisy realm is not recoverable the way a too-quiet
// one is.
// --------------------------------------------
extern uint32_t g_HsAmbientBucketRepliesPerMinute;
extern uint32_t g_HsAmbientBucketBurstCapacity;

// How long a bot waits after speaking ambiently before it may again --
// independent of the shared bucket above, which bounds the whole realm's
// ambient output, not any one bot's.
extern uint32_t g_HsAmbientBotCooldownSeconds;

// Same gate BotChain.RequireRealPlayer uses: off only makes sense for
// load-testing an empty realm. Party/raid need no such flag -- SayToParty/
// SayToRaid only reach real group members already (PlayerbotAI.cpp), so an
// all-bot group generates no packets regardless of this setting.
extern bool g_HsAmbientRequireRealPlayer;

// Per-surface enable. Trade and General reuse each channel's own MaxTier
// and RatePerMin bucket (HearthsideChat.Channel.<name>.*) rather than a
// third set of keys -- ambient on those two is gated by the shared bucket
// above plus that channel's own policy, nothing new to configure.
extern bool g_HsAmbientSayEnable;
extern bool g_HsAmbientPartyEnable;
extern bool g_HsAmbientRaidEnable;

// --------------------------------------------
// Fire chance for the three unprompted /say producers -- ambient musing
// (hs_ambient.cpp), openers (hs_opener.cpp) and proximity scenes
// (hs_script.cpp). Each is the last gate before a line is attempted, rolled
// after every cheaper in-memory test and before any DB query or bucket
// spend, so a bot that was never going to speak costs nothing.
//
// These were compiled constants until the settled-state gate
// (Hs_IsBotSettled, hs_rpgstate.h) landed. That gate is a large, hard-to-
// predict cut -- it depends on what fraction of a realm's bots happen to be
// resting or camping at any moment, which no amount of reading the code
// answers -- so the chances that compensate for it are the one thing here
// that has to be tunable against a live realm rather than guessed at
// compile time. All three defaults are raised from their pre-gate constants
// (ambient had no roll at all; openers 40; scenes 5) on the arithmetic that
// a narrower gate needs a higher roll to produce the same density, and all
// three are guesses to be corrected by observation.
//
// 0 disables that producer's /say path outright, which is the cheapest way
// to silence one surface without touching the others or the shared bucket.
// --------------------------------------------
extern uint32_t g_HsAmbientSayFireChancePercent;
extern uint32_t g_HsOpenerFireChancePercent;
extern uint32_t g_HsScriptProximityFireChancePercent;

// Gates the engagement follow-up feature (hs_engagement.h). Deliberately its
// own key rather than reusing MaxTier.Openers -- Openers is checked as
// HsTierAllows(ceiling, HsTier::Corpus), hardcoded, since openers have no
// generated-content path; reusing it here would silently cap follow-ups
// below inference on the documented MaxTier.Openers=corpus default, with
// nothing telling the operator that's what happened. Defaults "off", not
// "corpus" like Openers/BotToBot -- autonomous, GPU-doubling behavior that
// should be an explicit opt-in.
extern std::string g_HsMaxTierEngagementFollowUp;

// Gates the event-trigger surface (hs_event.h) -- bots reacting to deaths,
// dings, killing blows, rolls and duels. Its own key rather than reusing
// MaxTier.Ambient, which is documented as "unprompted ambient chatter near a
// player" and defaults to corpus: an event reaction is a *reaction to a
// stated fact*, not idle chatter, and it has no corpus path at all (a canned
// line about a specific death or roll would be wrong most of the time). So
// this is checked as HsTierAllows(ceiling, HsTier::Inference) and anything
// below that is silence, not a downgrade. Defaults "inference" -- unlike the
// engagement follow-up, an event reaction only fires on something that
// actually happened, and is bounded further by its own token budget below.
extern std::string g_HsMaxTierEvents;

// The event surface's own token budget (hs_queue.h's Hs_EventBucketTake),
// separate from Bucket.RepliesPerMinute so a busy dungeon's stream of
// deaths, loot and dings can never spend the budget a player's /say needed
// (PLAN-ARBITER.md §8). Deliberately much smaller than the reply bucket:
// this is ambient texture, and the failure mode of too much of it is bots
// narrating every corpse. Either key at 0 turns the surface off outright.
extern uint32_t g_HsEventBucketRepliesPerMinute;
extern uint32_t g_HsEventBucketBurstCapacity;

// --------------------------------------------
// Live bot-to-bot chains (hs_botchain.h), active only while
// MaxTier.BotToBot = "inference". Every hop is a full tier-2 call, so these
// five keys are the volume control on top of the global token bucket, which
// still caps everything.
//
// MaxDepth is a hard stop; the decayed chance is what normally ends a chain
// first. BaseChancePercent applies at depth 0 and is multiplied by
// DecayPercent/100 per additional hop (Hs_BotChainHopChancePercent), the same
// taper hs_engagement.cpp gives follow-up chains.
//
// ScopeCooldownSeconds paces whole chains, not the hops within one: it is
// only checked at depth 0, so a chain already under way keeps its turns
// conversationally prompt while a *new* chain in that group or channel has to
// wait out the rest period.
//
// RequireRealPlayer is the "is anyone actually there" gate -- a group with no
// human member, or a General channel with no human in it, is GPU spend against
// nobody's experience. Off only makes sense for load-testing an empty realm.
// --------------------------------------------
extern uint32_t g_HsBotChainMaxDepth;
extern uint32_t g_HsBotChainBaseChancePercent;
extern uint32_t g_HsBotChainDecayPercent;
extern uint32_t g_HsBotChainScopeCooldownSeconds;
extern bool     g_HsBotChainRequireRealPlayer;

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
//
// Three of these five are the module's only keys that do NOT take effect on
// `.reload config`, despite being re-read by it: Port, Bind and
// TimeoutSeconds are consumed once, by Hs_HttpServerStart, which runs only
// from HsHttpServerWorldScript::OnStartup (hs_main.cpp). By the time a
// reload rewrites them the listener is already bound, so the new values sit
// in these globals unused until the next worldserver start. PrivateKey and
// HttpControlEnable *do* take effect, because they are read per request --
// which makes the split more confusing rather than less, hence the
// RESTART REQUIRED banner on this section of conf.dist.
extern uint32_t     g_HsHttpServerPort;           // 0 = disabled; startup-only
extern std::string g_HsHttpServerBind;            // loopback by default; startup-only
extern std::string g_HsHttpServerPrivateKey;      // required when port != 0; live-reloads
extern uint32_t     g_HsHttpServerTimeoutSeconds; // startup-only
extern bool         g_HsHttpControlEnable;        // live-reloads

void LoadHearthsideChatConfig();

class HsConfigWorldScript : public WorldScript
{
public:
    HsConfigWorldScript();
    void OnStartup() override;

    // `.reload config` re-reads every HearthsideChat.* key into its global.
    // Gated on `reload` since OnStartup already covers the initial load.
    //
    // Re-read is not the same as takes-effect: HttpServerPort/Bind/
    // TimeoutSeconds are consumed once at startup and ignore the refreshed
    // value until the next restart (see their declarations above). Every
    // other key is read at its point of use and so genuinely live-reloads.
    void OnAfterConfigLoad(bool reload) override;
};

#endif // MOD_HS_CONFIG_H
