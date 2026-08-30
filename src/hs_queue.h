#ifndef MOD_HS_QUEUE_H
#define MOD_HS_QUEUE_H

#include "PlayerbotAIConfig.h" // NewRpgStatus (rpgInfo.GetStatus()), a live-activity fact
#include "hs_channel.h"        // HsChannelKind: §4.17 channel delivery/rate-limiting
#include "hs_topic_gate.h"     // HsTopicGateContext: §4.13 gear/group/instance/gold/zone facts

#include <cstdint>
#include <string>
#include <vector>

class Channel;
class Player;

// The runtime queue. A fixed worker pool of exactly one thread (slots in
// llama-server are a prompt cache, not a concurrency target, so requests are
// dispatched serially), backed by a bounded queue with a TTL, a global
// token bucket as the primary load ceiling, a per-bot cooldown on top of it,
// and a backend-down circuit breaker. This is the module's only stateful
// runtime subsystem besides delivery.

// Which surface a reply is delivered on. Selects both the PlayerbotAI send
// method at delivery (Say/Whisper/SayToParty/SayToRaid/SayToGuild) and the
// interaction_score weight (hs_identity.h). Party and Raid share one weight
// (both small-group, deliberate address) but need separate delivery methods
// since PlayerbotAI::SayToParty and ::SayToRaid are different calls.
enum class HsReplyChannel : uint8_t
{
    Say,
    Whisper,
    Party,
    Raid,
    Guild,
    Channel, // §4.17 global-channel chat; the specific channel is HsPendingReply::channelKind
};

// Lowercase name for logging/JSON, e.g. hs_metrics.cpp's per-channel
// breakdown rows and hs_http_server.cpp's status output.
const char* Hs_ReplyChannelName(HsReplyChannel channel);

// Starts the worker thread. Call once at worldserver startup.
void Hs_QueueStartup();

// Signals the worker to stop and joins it. Call once at worldserver shutdown.
void Hs_QueueShutdown();

// Attempts to admit one reactive-tier request. Applies, in order: the token
// bucket, the per-bot cooldown, the circuit breaker (silently, except for
// the one probe request let through per interval while open), and the
// bounded-queue depth cap. Returns false and does nothing further if any
// gate rejects: silence, not a queued retry.
//
// botName/senderName are carried through to the worker thread so the style
// pass (hs_style.h) can protect them from typo injection; the world thread
// already has both in hand at the call site (hs_handler.cpp). inCombat is
// likewise read at the call site (only the world thread touches Player*)
// and feeds the style pass's combat `care` offset. botLevel lets the worker
// thread restrict its archetype draw (hs_archetype.h) to the level-eligible
// pool. rpgStatus is `botAI->rpgInfo.GetStatus()` (mod-playerbots'
// NewRpgStatus: questing, grinding, outdoor PvP, resting, etc.), folded
// into the prompt as a short factual line so a bot can't claim to be doing
// something the realm's own state contradicts (e.g. "pvping" while
// mid-quest).
//
// isFollowUp is true only for a self-initiated engagement follow-up
// (hs_engagement.cpp): same admission gates as any reply, but the worker
// skips the interaction-score bump and the history write for these:
// bot-initiated, not a scored player utterance, and not useful prior-turn
// context.
//
// isEvent is the same idea for an event reaction (hs_event.cpp): also
// bot-initiated, so it suppresses the history append, the score bump, the
// engagement re-arm, and the distracted-reply roll exactly as isFollowUp
// does. It suppresses one thing more, Hs_EnsureFirstMeetingRecorded,
// because an event's "sender" is whoever the event happened around, which
// for a bot's own death or a bot-only group is another bot; recording a
// first meeting between two bots would seed identity state off something
// no player was part of. That single difference is why this is its own flag
// rather than a second caller passing isFollowUp.
//
// topicGate carries §4.13's remaining topic-gate facts (gear, group
// membership/leadership, in-instance, gold, zone), read at the call site
// like inCombat/botLevel/rpgStatus, then folded into the prompt as plain
// facts (hs_topic_gate.h) rather than an instruction.
//
// channelKind is only meaningful when `channel == HsReplyChannel::Channel`,
// exactly as on Hs_DeliverReflexReply below. It exists on this path because
// hs_botchain.h's live chain hop is the first tier-2 producer that can
// deliver into a global channel: every earlier channel reply was
// corpus-only and reached delivery through Hs_DeliverReflexReply, which has
// carried the kind since §4.17. Without it the worker's delivery push would
// take HsPendingReply's default and misdeliver every channel hop into Trade.
//
// chainScopeId/chainSeq tag a bot-to-bot chain hop (hs_botchain.h); 0 means
// "not a hop", which is every other caller. They are carried through to
// delivery so Hs_DeliverPending can drop a hop whose scope was taken over by
// a real player while it was still generating (the same stale-line problem
// Hs_CancelPendingFollowUpsFor solves for engagement follow-ups), but checked
// at delivery rather than cancelled at abort time, since a hop's scope is not
// keyed by the player whose message triggers the abort.
bool Hs_TryEnqueue(uint64_t botGuid, const std::string& botName, uint64_t senderGuid,
                    const std::string& senderName, HsReplyChannel channel, const std::string& userPrompt,
                    bool inCombat, uint8_t botLevel, NewRpgStatus rpgStatus,
                    const HsTopicGateContext& topicGate, bool isFollowUp, bool isEvent = false,
                    HsChannelKind channelKind = HsChannelKind::Trade,
                    uint64_t chainScopeId = 0, uint32_t chainSeq = 0);

// Claude/archive/PLAN-ARBITER.md §8: the event tier's own token bucket
// (HearthsideChat.Events.Bucket.*), independent of the tier-2 reply bucket
// Hs_TryEnqueue spends above. A busy dungeon generates deaths, loot and
// dings constantly; sharing one budget would let ambient reactions starve
// replies to players who actually spoke, which is the thing players notice
// most. Spent once per *event*, not per selected bot, and before any
// per-candidate work: a cheap early-out on an exhausted budget, the same
// shape Hs_ChannelBucketTake gives §4.17's per-channel buckets. Burst
// capacity is its own config key, since an event burst (a wipe) is a
// different shape from a chat burst. Returns false (caller does no further
// work) when the bucket is empty. An admitted event still has to clear
// Hs_TryEnqueue's own gates per bot on top of this.
bool Hs_EventBucketTake();

// Claude/archive/PLAN-AMBIENT.md §2: the shared *unprompted-speech* budget
// (HearthsideChat.Ambient.Bucket.*). Unlike every other bucket in this file,
// this one is not owned by a single surface: all three producers that
// speak on no trigger at all spend from it:
//
//   - hs_ambient.cpp's scan (dead air near a player)
//   - hs_opener.cpp's FireOpener (shared-context greeting)
//   - hs_script.cpp's two scene claims (/say and channel)
//
// One budget rather than three because ambient speech has no natural rate
// limiter. Every other surface is bounded by how often its trigger fires
// (players only talk so much, mobs only die so often), so a per-surface
// bucket there is a ceiling on something already self-limiting. These three
// are bounded only by the clock and the bot population, and three producers
// each individually tuned to "reasonable" still stack into constant noise.
// A realm's tolerance for bots talking to themselves is one quantity, so it
// gets one knob.
//
// Spent once per *line about to be spoken* (a script scene spends once at
// claim time for the whole multi-turn run, not per turn: the run is the
// unit a listener perceives). Returns false when the budget is empty; the
// caller falls silent rather than queuing, exactly as with the other
// buckets. Deliberately checked late, after the cheap eligibility filters
// but before the corpus query, so a denied line costs no DB work.
bool Hs_AmbientBucketTake();

// Session-cumulative grant/deny counts for the shared bucket above, for
// `.hearthside status` and the HTTP metrics route. Saturation here is the
// signal that the three producers are competing rather than coexisting --
// the number to look at before raising Ambient.Bucket.RepliesPerMinute.
struct HsAmbientBucketStats
{
    uint64_t grantedCount;
    uint64_t deniedCount;
};
HsAmbientBucketStats Hs_AmbientBucketStatsSnapshot();

// Delivers any replies the worker has finished since the last call. Must be
// called once per world tick, from the world thread only: this is the
// only place a Player*/PlayerbotAI* is ever touched for this subsystem,
// avoiding the data race of calling botAI->Say() from a background thread.
void Hs_DeliverPending();

// Drops any not-yet-delivered engagement follow-up (hs_engagement.h) queued
// for this player, across every bot. Called when they send a new message
// before a scheduled follow-up's deliverAt, so a stale one can't arrive
// after they've already said something else. Direct replies and the
// self-correction follow-up are never cancelled by this; only entries
// tagged isFollowUp are eligible.
void Hs_CancelPendingFollowUpsFor(uint64_t senderGuid);

// Drops every conversation-history entry this bot is a party to, across all
// players. Called when the bot is detected to have been *reset* by
// mod-playerbots' recycler (hs_event.cpp's level-drop branch).
//
// The recycler resets a character in place: RandomBotLevelMgr::ResetBot
// keeps the same GUID and the same name, and knocks the level back down a
// bracket. So this is not "a different person now" (the GUID never changes
// hands, and a logout/login is not a reset), but the bot's level, gear, zone
// and goals have all just been rewritten, and the last thing it said about
// any of them is no longer true of it. Prior-turn context from before that
// point is worse than no context, so it goes.
//
// Deliberately narrow: only the history, which is the only one of this
// module's in-memory maps that holds *content* about a prior relationship
// rather than a timestamp. A stale 8-second reply cooldown surviving a reset
// is not worth a hook.
void Hs_ForgetBotHistory(uint64_t botGuid);

// Delivers a tier-0 reflex reply, and also a grounded-answer reply: both are
// "answer without the GPU" paths that need identical no-bucket, no-cooldown,
// no-worker-thread, no-history/identity-write delivery, so grounded answers
// reuse this function rather than duplicating it. Both callers only reach
// this after the arbiter selected the bot, and score nothing: tier 0 stays
// completely free of identity side effects. Style-pass `text` before
// calling this (hs_style.h); this function delivers it verbatim after a
// short randomized delay, reusing the same delivery-queue drain
// (Hs_DeliverPending) rather than duplicating Say()/Whisper() dispatch. A
// call with empty `text` is a no-op (e.g. Silent BotQuestion mode, or a
// matched PersonalProbe pool entry with no reply).
//
// `channelKind` is only meaningful when `channel == HsReplyChannel::Channel`
// (§4.17's corpus-fallback channel reply, hs_handler.cpp's Channel* hook),
// ignored otherwise, default value arbitrary.
void Hs_DeliverReflexReply(uint64_t botGuid, uint64_t senderGuid, HsReplyChannel channel, const std::string& text,
                            HsChannelKind channelKind = HsChannelKind::Trade);

// §4.17: attempts to spend one token from this channel's own rate-limit
// bucket (HearthsideChat.Channel.<name>.RatePerMin), independent of the
// tier-2 GPU bucket Hs_TryEnqueue gates above: corpus-fallback channel
// replies never reach Hs_TryEnqueue at all (zero GPU work, same reasoning
// hs_corpus.h gives for Hs_SelectCorpusLine), so without this a channel with
// no proximity bound would have nothing capping reply volume. Returns false
// (spend nothing, caller does no further work) if that channel's bucket is
// empty. Checked by hs_handler.cpp's Channel* hook before building any
// candidate list: a cheap early-out on a throttled channel.
bool Hs_ChannelBucketTake(HsChannelKind kind);

// §4.17: resolves the live Channel* a bot should speak into for delivery,
// building the same name Player::UpdateLocalChannels does: the zone name for
// General/LocalDefense, and for the city-scoped Trade/GuildRecruitment the
// LANG_CHANNEL_CITY acore_string rather than AreaTable 3459. That function is
// what actually joins anyone to these channels, so matching it is what makes
// the lookup hit; see the block in the .cpp for why the AreaID 3459 route
// this used to take (and that PlayerbotMgr.cpp still takes) resolves to
// nullptr instead. Note ChannelMgr::GetChannel is a pure name lookup and does
// *not* test membership, so a hit here proves the channel exists, not that the
// bot is in it: every caller that needs the latter checks
// Player::IsInChannel itself. Called fresh at delivery time from the bot's
// *then-current* zone by both Hs_DeliverPending (a corpus-fallback channel
// reply) and hs_script.cpp's channel-script delivery, not a name captured
// earlier: the bot may have moved zones during the typing delay for a
// zone-scoped channel (General/LocalDefense). Returns nullptr if the bot no
// longer resolves to that channel instance; caller must not misdeliver.
Channel* Hs_ResolveChannelForDelivery(Player* bot, HsChannelKind kind);

// Seconds since this bot's last *successfully delivered* reply, or
// UINT32_MAX if never. Read-only query for hs_arbiter's recent-speaker
// weighting; the hard cooldown gate itself lives in Hs_TryEnqueue.
uint32_t Hs_SecondsSinceLastReply(uint64_t botGuid);

// Read-only status for the `.hearthside status` GM command.
bool     Hs_IsBackendDown();
uint32_t Hs_PendingQueueDepth();

// Idle signal for the generator: true only when the reactive worker has
// nothing queued and isn't mid-request. The generator checks this before
// every generation call so it yields immediately rather than competing with
// a live reply for the GPU. No need to poll /slots or NVML when the
// module's own in-flight count already answers the question.
bool Hs_IsReactiveIdle();

// The most recent *pre-style* reply this bot sent (empty if none yet, or if
// the bot has sent one but never through the reactive tier). Backs
// `.hearthside capture`: the pre-style text is captured specifically so the
// styled/typo'd version a player saw doesn't bake one bot's misspelling
// into the corpus for every bot that ever draws the row. Keyed by bot name
// since that's what a GM types; overwritten on every successful reactive
// reply, so "capture" always means the last one, never an accumulating
// history.
std::string Hs_LastPreStyleReply(const std::string& botName);

// ---- §4.19 fuller metrics -------------------------------------------------
// Recorded once per completed tier-2 request (replied or silent) from
// WorkerLoop; read by hs_metrics.cpp on its periodic sample. In-memory only,
// like the rest of this file's state; hside_metrics is what gives these
// history across restarts.

// Percentiles over a rolling window of the most recent reactive-tier call
// latencies (Hs_CallLLM's own wall time, not queue wait). All zero if no
// samples have landed yet.
struct HsLatencyPercentiles
{
    uint32_t p50Ms;
    uint32_t p95Ms;
    uint32_t p99Ms;
};
HsLatencyPercentiles Hs_ReactiveLatencyPercentiles();

// Mean assembled-prompt character length by identity ring (1=stranger,
// 2=known, 3=carded): ring is the dominant driver of injected persona
// text (§4.12), so this is where a prefill-budget regression would show up
// first. Zero for a ring with no samples yet.
struct HsPromptCharsByRing
{
    uint32_t ring1Mean;
    uint32_t ring2Mean;
    uint32_t ring3Mean;
};
HsPromptCharsByRing Hs_PromptCharsByRing();

// Reply-vs-silence counts since this worldserver process started, keyed by
// archetype and by delivery channel. Not surfaced by `.hearthside status`
// (too wide for a chat window, same reasoning hs_metrics.h already gives
// for the metrics table itself). The HTTP /api/metrics route is the
// consumer.
struct HsArchetypeReplyCounts
{
    std::string enumName;
    uint32_t    repliedCount;
    uint32_t    silentCount;
};
std::vector<HsArchetypeReplyCounts> Hs_ArchetypeReplyCountsSnapshot();

struct HsChannelReplyCounts
{
    HsReplyChannel channel;
    uint32_t        repliedCount;
    uint32_t        silentCount;
};
std::vector<HsChannelReplyCounts> Hs_ChannelReplyCountsSnapshot();

// ---- TTL drop rate / token-bucket saturation ------------------------------
// Session-cumulative counts (since worldserver process start, like the
// reply/silence counts above), not a rolling window. Named as a gap in
// Claude/archive/ISSUES.md ("TTL drop rate and per-surface token-bucket saturation
// still aren't tracked anywhere") while building the rest of §4.19's fuller
// metrics; built as a follow-up once the operator asked for it explicitly.

// droppedCount of processedCount total dequeued requests were stale
// (age > HearthsideChat.Queue.TTLSeconds) when the worker reached them.
struct HsTtlDropStats
{
    uint64_t droppedCount;
    uint64_t processedCount;
};
HsTtlDropStats Hs_TtlDropStatsSnapshot();

// deniedCount of attemptCount admission attempts found the bucket empty.
// Global (tier-2) bucket only. See Hs_ChannelBucketSaturationSnapshot for
// §4.17's per-channel buckets, which are independent of this one.
struct HsBucketSaturationStats
{
    uint64_t deniedCount;
    uint64_t attemptCount;
};
HsBucketSaturationStats Hs_GlobalBucketSaturationSnapshot();

struct HsChannelBucketSaturationStats
{
    HsChannelKind kind;
    uint32_t       grantedCount;
    uint32_t       deniedCount;
};
std::vector<HsChannelBucketSaturationStats> Hs_ChannelBucketSaturationSnapshot();

#endif // MOD_HS_QUEUE_H
