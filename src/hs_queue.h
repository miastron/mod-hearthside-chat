#ifndef MOD_HS_QUEUE_H
#define MOD_HS_QUEUE_H

#include "PlayerbotAIConfig.h" // NewRpgStatus (rpgInfo.GetStatus()) -- live-activity fact

#include <cstdint>
#include <string>

// The runtime queue. A fixed worker pool of exactly one thread -- slots in
// llama-server are a prompt cache, not a concurrency target, so requests are
// dispatched serially -- backed by a bounded queue with a TTL, a global
// token bucket as the primary load ceiling, a per-bot cooldown on top of it,
// and a backend-down circuit breaker. This is the module's only stateful
// runtime subsystem besides delivery.

// Starts the worker thread. Call once at worldserver startup.
void Hs_QueueStartup();

// Signals the worker to stop and joins it. Call once at worldserver shutdown.
void Hs_QueueShutdown();

// Attempts to admit one reactive-tier request. Applies, in order: the token
// bucket, the per-bot cooldown, the circuit breaker (silently, except for
// the one probe request let through per interval while open), and the
// bounded-queue depth cap. Returns false and does nothing further if any
// gate rejects -- silence, not a queued retry.
//
// botName/senderName are carried through to the worker thread so the style
// pass (hs_style.h) can protect them from typo injection; the world thread
// already has both in hand at the call site (hs_handler.cpp). inCombat is
// likewise read at the call site (only the world thread touches Player*)
// and feeds the style pass's combat `care` offset. botLevel lets the worker
// thread restrict its archetype draw (hs_archetype.h) to the level-eligible
// pool. rpgStatus is `botAI->rpgInfo.GetStatus()` (mod-playerbots'
// NewRpgStatus -- questing, grinding, outdoor PvP, resting, etc.), folded
// into the prompt as a short factual line so a bot can't claim to be doing
// something the realm's own state contradicts (e.g. "pvping" while
// mid-quest).
//
// isFollowUp is true only for a self-initiated engagement follow-up
// (hs_engagement.cpp): same admission gates as any reply, but the worker
// skips the interaction-score bump and the history write for these --
// bot-initiated, not a scored player utterance, and not useful prior-turn
// context.
bool Hs_TryEnqueue(uint64_t botGuid, const std::string& botName, uint64_t senderGuid,
                    const std::string& senderName, bool isWhisper, const std::string& userPrompt,
                    bool inCombat, uint8_t botLevel, NewRpgStatus rpgStatus, bool isFollowUp);

// Delivers any replies the worker has finished since the last call. Must be
// called once per world tick, from the world thread only -- this is the
// only place a Player*/PlayerbotAI* is ever touched for this subsystem,
// avoiding the data race of calling botAI->Say() from a background thread.
void Hs_DeliverPending();

// Drops any not-yet-delivered engagement follow-up (hs_engagement.h) queued
// for this player, across every bot -- called when they send a new message
// before a scheduled follow-up's deliverAt, so a stale one can't arrive
// after they've already said something else. Direct replies and the
// self-correction follow-up are never cancelled by this; only entries
// tagged isFollowUp are eligible.
void Hs_CancelPendingFollowUpsFor(uint64_t senderGuid);

// Delivers a tier-0 reflex reply, and also a grounded-answer reply: both are
// "answer without the GPU" paths that need identical no-bucket, no-cooldown,
// no-worker-thread, no-history/identity-write delivery, so grounded answers
// reuse this function rather than duplicating it. Both callers only reach
// this after the arbiter selected the bot, and score nothing -- tier 0 stays
// completely free of identity side effects. Style-pass `text` before
// calling this (hs_style.h); this function delivers it verbatim after a
// short randomized delay, reusing the same delivery-queue drain
// (Hs_DeliverPending) rather than duplicating Say()/Whisper() dispatch. A
// call with empty `text` is a no-op (e.g. Silent BotQuestion mode, or a
// matched PersonalProbe pool entry with no reply).
void Hs_DeliverReflexReply(uint64_t botGuid, uint64_t senderGuid, bool isWhisper, const std::string& text);

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
// a live reply for the GPU -- no need to poll /slots or NVML when the
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

#endif // MOD_HS_QUEUE_H
