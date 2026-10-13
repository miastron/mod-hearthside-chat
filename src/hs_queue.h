#ifndef MOD_HS_QUEUE_H
#define MOD_HS_QUEUE_H

#include "PlayerbotAIConfig.h" // NewRpgStatus (rpgInfo.GetStatus()), a live-activity fact
#include "hs_channel.h"        // HsChannelKind: §4.17 channel delivery/rate-limiting
#include "hs_topic_gate.h"     // HsTopicGateContext: §4.13 gear/group/instance/gold/zone facts

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class Channel;
class ObjectGuid;
class Player;
struct HsCardSnapshot;

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
// breakdown rows.
const char* Hs_ReplyChannelName(HsReplyChannel channel);

// Starts the worker thread. Call once at worldserver startup.
void Hs_QueueStartup();

// Signals the worker to stop and joins it. Call once at worldserver shutdown.
void Hs_QueueShutdown();

// One reactive-tier request. Hs_MakeReplyRequest fills everything that has
// to be read off a Player* on the world thread; the caller then sets, by
// name, the fields that say what kind of request this is.
//
// A struct rather than the seventeen positional parameters Hs_TryEnqueue
// used to take. Most of those were bools and small enums passed by position
// behind a run of defaults, so inserting one reordered its neighbours'
// meaning: it did on 2026-09-20, and failed to compile only because the
// rebound argument happened to be an HsChannelKind landing on a bool.
struct HsReplyRequest
{
    uint64_t       botGuid    = 0;
    std::string    botName;       // style pass: protected from typo injection
    uint64_t       senderGuid = 0;
    std::string    senderName;    // style pass: protected from typo injection
    HsReplyChannel channel    = HsReplyChannel::Say;
    std::string    prompt;        // what is being answered; see triggerIsStateLine

    // ---- bot state, sampled on the world thread by Hs_MakeReplyRequest ----
    // Only the world thread may touch Player*/PlayerbotAI*, so the worker
    // reads these values instead of the objects.
    bool               inCombat   = false;    // style pass: combat `care` offset
    uint8_t            botLevel   = 0;        // Hs_BumpInteractionScore's card-retirement check
    NewRpgStatus       rpgStatus  = RPG_IDLE; // mod-playerbots' live activity, stated in the prompt as a fact
    bool               botSettled = false;    // Hs_IsBotSettled (hs_rpgstate.h); gates the distracted reply only
    HsTopicGateContext topicGate;             // §4.13 gear/group/instance/gold/zone facts (hs_topic_gate.h)
    bool               senderIsAlly = false;  // sender is in the bot's group or guild right now: the live half of
                                              // the expert-answer gate (Hs_HasBondWith is the durable half)

    // ---- what kind of request this is ----

    // A self-initiated engagement follow-up (hs_engagement.cpp): same
    // admission gates as any reply, but the worker skips the
    // interaction-score bump and the history write -- bot-initiated, not a
    // scored player utterance, and not useful prior-turn context.
    bool isFollowUp = false;

    // The sender is not a real player: an event reaction (hs_event.cpp),
    // whose "sender" is whoever the event happened around, or a bot-to-bot
    // chain hop (hs_botchain.cpp). Suppresses everything isFollowUp does
    // (history, score bump, engagement re-arm, distracted reply) plus
    // Hs_EnsureFirstMeetingRecorded, since "met" between two bots is not a
    // fact about any player.
    bool isEvent = false;

    // `prompt` is a synthetic line describing something that happened ("You
    // were out on your own and have just been killed."), not something
    // anybody said. The worker states it as a fact alongside the bot's other
    // state and puts a react-instruction in the slot a player's message would
    // occupy. Deliberately not folded into isEvent: a chain hop is isEvent
    // too, and its trigger is a real line another bot said, which belongs in
    // the trigger slot untouched.
    bool triggerIsStateLine = false;

    // Only meaningful when channel == HsReplyChannel::Channel, exactly as on
    // Hs_DeliverReflexReply. A live chain hop is the one tier-2 producer
    // that can deliver into a global channel.
    HsChannelKind channelKind = HsChannelKind::Trade;

    // A bot-to-bot chain hop (hs_botchain.h); 0 = not a hop. Carried to
    // delivery so Hs_DeliverPending can drop a hop whose scope a real player
    // took over while it was still generating. Checked at delivery rather
    // than cancelled at abort time, since a hop's scope is not keyed by the
    // player whose message triggers the abort.
    uint64_t chainScopeId = 0;
    uint32_t chainSeq     = 0;

    // ---- cloud facts (hs_cloud.h) ----
    // Set only by hs_handler.cpp, for a real player's own chat (or a GM ask
    // with Cloud.AllowGmAsk): the account the daily limit is counted against.
    // 0 = this request may never reach the cloud. Hs_TryEnqueue and
    // hs_cloud.cpp each re-check it independently; see the plan's fail-safe.
    uint32_t    cloudAccountId = 0;
    // Filled by the cloud thread: "Title -- sentence." for the prompt, or
    // empty. cloudDone marks a request that has already been through it, so
    // the worker routes each request at most once.
    std::string cloudFact;
    bool        cloudDone = false;
};

// World thread only. Fills the identity and bot-state halves of a request
// from the live objects; the caller sets the kind flags afterwards. The one
// place the topic-gate facts, the rpg status and the settled test are read
// for a reply -- four callers used to spell the same reads out by hand.
HsReplyRequest Hs_MakeReplyRequest(Player* bot, Player* sender, HsReplyChannel channel, const std::string& prompt);
// The bot's primary professions with their skill values, in a fixed order;
// WotLK allows at most two. Shared by the topic-gate facts and the grounded
// "what professions" answer.
std::vector<std::pair<std::string, uint32_t>> Hs_PrimaryProfessions(Player* bot);

// Same, for a sender known only by GUID and name: `.hearthside ask` from the
// console speaks as a character that need not be online.
HsReplyRequest Hs_MakeReplyRequest(Player* bot, uint64_t senderGuid, const std::string& senderName,
                                   HsReplyChannel channel, const std::string& prompt);

// Attempts to admit one reactive-tier request. Applies, in order: the token
// bucket, the per-bot cooldown, the circuit breaker (silently, except for
// the one probe request let through per interval while open), and the
// bounded-queue depth cap. Returns false and does nothing further if any
// gate rejects: silence, not a queued retry.
bool Hs_TryEnqueue(HsReplyRequest request);

// Runs `work` on the world thread, from the next Hs_RunDeferredWork call
// (HsDeliveryWorldScript::OnUpdate, every world tick).
//
// For PlayerScript/GroupScript/GuildScript hooks. Most of them fire from
// inside Map::Update -- Unit::Kill, Player::Update, InstanceScript encounter
// credit -- and Map::Update runs on MapUpdate.Threads worker threads, several
// maps at once (8 on the test realm). From there it is not safe to walk
// ObjectAccessor::GetPlayers(), read a player on another map, or touch the
// module's world-thread-only state, and not cheap to block on the database.
// World::Update runs the map update before OnWorldUpdate, so deferred work
// normally runs later in the same tick.
//
// The closure must capture GUIDs and plain values, never a Player*, Group*,
// Guild* or anything else the hook was handed: re-resolve on the world
// thread, where the object may since have gone. Returns false (the work is
// dropped) when the backlog cap is reached, which only a stalled world
// thread should ever hit. Callable from any thread.
bool Hs_DeferToWorldThread(std::function<void()> work);

// World thread only: runs everything Hs_DeferToWorldThread queued so far.
void Hs_RunDeferredWork();

// The re-resolve half of that contract: a GUID captured on a map thread,
// back to a live Player* on the world thread, or nullptr if they left the
// world in between.
Player* Hs_FindInWorld(ObjectGuid guid);

// The event tier's own token bucket (HearthsideChat.Events.Bucket.*), apart
// from the reply bucket so a busy dungeon's deaths and dings can't starve
// replies to players who spoke. Spent once per event, before any
// per-candidate work; false means do nothing further. An admitted event
// still has to clear Hs_TryEnqueue's per-bot gates.
bool Hs_EventBucketTake();

// True when Hs_EventBucketTake would fail right now, for a caller with
// expensive work to do before it could take (the level-up witness scan).
bool Hs_EventBucketExhausted();

// Review C2: refund for an event whose EligibleBot filtering left no actor
// able to speak, so the budget paid for nothing.
void Hs_EventBucketRefund();

// The shared unprompted-speech budget (HearthsideChat.Ambient.Bucket.*),
// spent by the three producers that speak on no trigger: ambient musing,
// openers, and scripted scenes (once per scene, not per turn). Those are
// bounded only by the clock, and a realm's tolerance for bots talking to
// themselves is one quantity, so it gets one knob. Taken after the cheap
// eligibility filters but before the corpus query; false means stay silent.
bool Hs_AmbientBucketTake();

// Returns a token for a line nothing was ever going to speak (an empty
// reserve, no candidate, a script with no turns). Not for a line dropped
// after the budget said yes: that is silence a player experiences.
void Hs_AmbientBucketRefund();

// Session-cumulative grant/deny counts for the shared bucket above, for
// `.hearthside status` and the metrics sampler. Saturation here is the
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
// Also drops two other pieces of per-bot content, on the same staleness
// argument: this bot's recent-public-utterance context
// (Hs_RecentUtteranceContext), since a pre-reset ad is no longer true of the
// character, and its ambient-experience ring (Hs_ForgetExperience,
// hs_experience.h), since the quests it finished and the zones it walked
// belong to a character that has just had its level, gear, zone and goals
// rewritten.
//
// Deliberately narrow beyond those three. Every other in-memory map in this
// file holds only a timestamp, not content about a prior relationship, the
// bot's own words, or what it has been doing, so a stale 8-second reply
// cooldown surviving a reset is not worth a hook. The rule is "drop what
// makes a claim about who this character is", not "drop everything keyed by
// the GUID".
void Hs_ForgetBotHistory(uint64_t botGuid);

// Adds a canned exchange (reflex or grounded) to this bot-player pair's
// history, so the model's next reply knows a conversation is under way.
// Before 2026-10-07 only model replies were recorded, and "sup" -> "hey, not
// much" followed by "wyd?" reached the model as an opening line.
void Hs_RecordExchange(uint64_t botGuid, uint64_t senderGuid, const std::string& trigger, const std::string& reply);

// Delivers a reply that never touched the GPU (reflex, grounded, corpus,
// opener, ambient, canned event line): no bucket, no cooldown, no worker
// thread, no history or identity write. `text` must already be styled
// (hs_style.h); it is delivered verbatim after a short randomized delay
// through the same queue drain (Hs_DeliverPending) as every other line. An
// empty `text` is a no-op (Silent BotQuestion mode, a PersonalProbe pool
// entry with no reply). `channelKind` only matters when `channel` is
// HsReplyChannel::Channel. `source` names the tier for the conversation log.
void Hs_DeliverReflexReply(uint64_t botGuid, uint64_t senderGuid, HsReplyChannel channel, const std::string& text,
                            HsChannelKind channelKind = HsChannelKind::Trade, char const* source = "canned");

// Styles `text` in this bot's voice and hands it to Hs_DeliverReflexReply.
// Returns what was delivered: "" when styling emptied it and nothing was sent.
std::string Hs_StyleAndDeliver(Player* bot, uint64_t senderGuid, const std::string& listenerName,
                               const std::string& text, HsReplyChannel channel, HsChannelKind channelKind,
                               bool inCombat, char const* source);

// A pre-written line (corpus, opener, ambient): card placeholders for a
// carded bot, then the universal ones (%zone, %item_link...), then
// Hs_StyleAndDeliver. False, with nothing sent, when a placeholder cannot be
// resolved -- silence rather than an untrue claim (§4.13) -- or styling
// empties the line. `card` is the bot's snapshot if the caller already has
// one; otherwise it is looked up, and only when the line holds a '%'.
bool Hs_DeliverCorpusLine(Player* bot, std::string line, uint64_t senderGuid, const std::string& listenerName,
                          HsReplyChannel channel, HsChannelKind channelKind, bool inCombat, char const* source,
                          HsCardSnapshot const* card = nullptr);

// The conversation log (HearthsideChat.ConversationLog.Enable): one JSON line
// on module.hearthside.convo per player line that reaches a bot. Bot lines are
// logged by Hs_DeliverPending as they are delivered, with their source and,
// for a model line, the system block, history and trigger it was given.
void Hs_ConvoLogPlayerLine(uint64_t botGuid, const std::string& botName, uint64_t playerGuid,
                           const std::string& playerName, HsReplyChannel channel, const std::string& text);

// A channel script turn, spoken outside Hs_DeliverPending, so the
// conversation log holds every General line; runId groups a scene's turns.
void Hs_ConvoLogScriptLine(uint64_t botGuid, const std::string& botName, uint64_t toGuid, const std::string& toName,
                           const std::string& text, uint32_t runId);

// Records that this bot said `text` somewhere with no bounded/membership
// audience (Say or Channel; see hs_queue.cpp's g_RecentUtterances comment
// for why Whisper/Party/Raid/Guild are excluded). Called from every actual
// delivery point regardless of tier -- Hs_DeliverPending's dispatch loop
// covers reflex/grounded/corpus/ambient/opener/reactive, hs_script.cpp's two
// direct Say() sites call it themselves -- but for a reply's primary line
// only: the "sorry, was afk" filler and a `*correction` fragment are flavor
// attached to a line, and "You recently said: \"*healer\"" is noise in the
// next prompt, not context. A no-op on empty text. This is not
// a relationship or identity write (contrast hs_identity_store.h/
// hs_memory_store.h): it only ever feeds this same bot's own future
// Hs_RecentUtteranceContext.
void Hs_RecordBotUtterance(uint64_t botGuid, const std::string& text);

// Short first-person context block of this bot's most recent public lines
// (empty string if it hasn't said anything eligible lately), meant to be
// concatenated onto the archetype/persona line passed to Hs_CallLLM --
// see hs_queue.cpp's reactive-path personaLine assembly for the existing
// precedent (cardSnapshot/rpgHint/topicGate already fold in there rather
// than widening Hs_CallLLM's signature).
std::string Hs_RecentUtteranceContext(uint64_t botGuid);

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

// Review C2: the channel counterpart of Hs_AmbientBucketRefund. The
// channel-scene path spends *both* this bucket and the shared ambient one,
// and the shared take can deny after the channel token is already gone --
// that double-spend is the case this exists for.
void Hs_ChannelBucketRefund(HsChannelKind kind);

// §4.17: resolves the live Channel* a bot should speak into for delivery,
// building the same name Player::UpdateLocalChannels does: the zone name for
// General, and for the city-scoped Trade the LANG_CHANNEL_CITY acore_string rather than AreaTable 3459. That function is
// what actually joins *real players*, so matching it is what makes the
// lookup hit the instance worth reaching. It is deliberately not the name
// PlayerbotMgr.cpp builds: its AreaID 3459 route yields an empty suffix, so
// bots are all joined to a second, differently-named channel sharing the
// same DBC id. See the block in the .cpp; Hs_DeliverPending bridges the two.
//
// Note ChannelMgr::GetChannel is a pure name lookup and does *not* test
// membership, so a hit here proves the channel exists, not that the bot is
// in it -- with one exception: for the city-scoped Trade this returns
// nullptr unless the bot's zone carries AREA_FLAG_SLAVE_CAPITAL, because its
// name is a constant and the lookup alone would otherwise resolve every bot
// on the realm into the one live instance. See the .cpp for why the zone is
// the only membership signal available there.
//
// Called fresh at delivery time from the bot's *then-current*
// zone by both Hs_DeliverPending (a corpus-fallback channel reply) and
// hs_script.cpp's channel-script delivery, not a name captured earlier: the
// bot may have moved zones during the typing delay for a zone-scoped channel
// (General). Returns nullptr if the bot no longer resolves to
// that channel instance; caller must not misdeliver.
// sendPacketOnMiss forwards to ChannelMgr::GetChannel's own `pkt` (default
// true there, matching every pre-existing caller of this function): on a
// miss it sends the passed-in `bot` a "not on channel" client message. Every
// existing caller passes a bot, so that message lands on nobody. The
// exception is Hs_IsInChannelInstance below, which resolves a candidate who
// may be a real player who has never done anything wrong: it passes false, or
// probing that player's zone would occasionally hand them a spurious system
// message purely because a corpus/channel scan ran.
Channel* Hs_ResolveChannelForDelivery(Player* bot, HsChannelKind kind, bool sendPacketOnMiss = true);

// True if `player` is in the specific channel *instance* `channel` -- the test
// every candidate scan in this module wants, and the one that has to be
// written out longhand because the core will not answer it directly.
//
// Channel::IsOn(GUID) is that answer, and it is private on this AzerothCore
// revision (Channel.h), as are GetPlayerFlags and Player::m_channels. So only
// two questions can legally be asked, and neither alone is the one we want:
//
//   - Hs_ResolveChannelForDelivery(player, kind): "which instance does this
//     player's *current zone* map to". Says nothing about whether they ever
//     joined it -- ChannelMgr::GetChannel is a pure name lookup.
//   - Player::IsInChannel(channel): "is some channel in this player's joined
//     list", but it compares GetChannelId(), the DBC *type* (Player.cpp).
//     Every zone's General shares one type id, so on its own it cannot tell
//     one zone's instance from another's -- which is exactly the bug that
//     made General chatter fire into zones with nobody in them while the
//     player standing in Orgrimmar heard nothing.
//
// This asks both and requires both, which is precisely what the private
// IsOn would have answered: joined-something-of-this-type AND this-zone-
// resolves-to-this-instance. Neither half is redundant. Drop the first and a
// player who typed `/leave General` while standing in the zone still counts
// as an audience, so bots perform to an empty room -- a milder cousin of the
// original bug. Drop the second and every zone's General collapses into one,
// which was the original bug itself.
//
// IsInChannel is tested first because it is the cheap half (a walk of a
// short list) and it short-circuits the resolve, which the channel scans
// call once per candidate and which their own comments flag as by far the
// most expensive test in the loop.
//
// 2026-10-07: Player::IsInChannel no longer exists. It was a playerbots-only
// core patch, and the 2026-09-23 core merge (azerothcore-wotlk f1b18a2ea)
// dropped it. Half one is now Hs_PlayerIsOnChannel below, which answers the
// stricter question this comment wished for: membership of this exact
// instance. Half two is kept as it was.
//
// Self-resolved call sites (hs_ambient.cpp, hs_script.cpp, hs_command.cpp)
// deliberately do not route through this function: they need the resolved
// Channel* itself to group by instance, and already spell out the same two
// halves with a single resolve. Calling this there would resolve twice.
bool Hs_IsInChannelInstance(Player* player, HsChannelKind kind, Channel* channel);

// True if `player` is a member of this exact channel instance -- the core's
// own Channel::IsOn, which is private. Reached through the standard
// explicit-instantiation access idiom in hs_queue.cpp rather than a core
// patch, so a future core merge cannot silently delete it the way it deleted
// Player::IsInChannel; if Channel::IsOn is ever renamed the build breaks
// loudly at that one line instead.
bool Hs_PlayerIsOnChannel(Player const* player, Channel const* channel);

// Why a channel scan found nothing. Every one of these is a silent `return`
// at the call site, and an operator seeing no Trade or General traffic
// cannot otherwise tell them apart -- so the reason comes back rather than
// being logged here, letting each surface word its own trace.
enum class HsChannelScanMiss
{
    None,
    NoBotResolved,          // no eligible bot resolved into this channel at all
    NoRealPlayerOnline,     // requireRealPlayer and nobody human is online
    NoInstanceWithAudience, // instances exist, none with both enough bots and a listener
};

struct HsChannelScanPick
{
    Channel*             channel = nullptr;
    std::vector<Player*> bots;                                 // empty unless miss == None
    HsChannelScanMiss    miss    = HsChannelScanMiss::NoBotResolved;
};

// Picks one instance of a global channel that can carry a line, plus the
// eligible bots standing in it.
//
// Global channels are one Channel object per zone, so "is a bot in Trade" is
// never the question -- "is a bot in *this player's* Trade" is. The scan
// walks the online population once, groups bots by the Channel* they resolve
// to (the same zone-qualified resolution delivery itself uses, which is what
// makes same-Channel* equivalent to same-instance), drops instances without
// enough bots or without a human to hear them, and picks uniformly among
// what is left rather than taking whichever instance enumerated first.
//
// Review item 22: hs_ambient.cpp's TryAmbientChannel and hs_script.cpp's
// TryFireChannelScript were near line-for-line copies of all of that, each
// citing the other in comments. What actually differs between them is the
// four arguments below -- how many bots an instance needs (one to muse, two
// to hold a scene), whether a listener is required at all
// (Ambient.RequireRealPlayer), how many bots are worth collecting per
// instance, and which bots that surface considers eligible. HsChannelPolicy's
// excludeGroupedBots is applied here for every caller (review item 23).
//
// botEligible runs after the cheap shared gates and before the resolve,
// which is the expensive test (a DBC lookup plus a ChannelMgr string match).
HsChannelScanPick Hs_PickChannelInstance(HsChannelKind kind, size_t minBots, bool requireRealPlayer,
                                          size_t maxBotsPerInstance,
                                          const std::function<bool(Player*)>& botEligible);

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
// (too wide for a chat window); the metrics sampler writes them to
// hside_metrics_breakdown for the acore-web-reg dashboard.
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
