#ifndef MOD_HS_QUEUE_H
#define MOD_HS_QUEUE_H

#include "PlayerbotAIConfig.h" // NewRpgStatus (rpgInfo.GetStatus()) -- §4.13-adjacent live-activity fact, new 2026-08-21

#include <cstdint>
#include <string>

// PLAN.md §4.3 — the runtime queue. Fixed worker pool of exactly one thread
// (decided: slots in llama-server are a prompt cache, not a concurrency
// target — requests are dispatched serially), a bounded queue with a TTL, a
// global token bucket as the primary load ceiling, a per-bot cooldown on top
// of it, and a backend-down circuit breaker. This is the module's only
// stateful runtime subsystem besides delivery.

// Starts the worker thread. Call once at worldserver startup.
void Hs_QueueStartup();

// Signals the worker to stop and joins it. Call once at worldserver shutdown.
void Hs_QueueShutdown();

// Attempts to admit one reactive-tier request. Applies, in order: the token
// bucket, the per-bot cooldown, the circuit breaker (silently, except for
// the one probe request let through per interval while open), and the
// bounded-queue depth cap. Returns false and does nothing further if any
// gate rejects — §1's retreat rule: silence, not a queued retry.
//
// botName/senderName are carried through to the worker thread so the style
// pass (hs_style.h, §4.11) can protect them from typo injection; the world
// thread already has both in hand at the call site (hs_handler.cpp).
// inCombat is likewise read at the call site (only the world thread touches
// Player*) and feeds the style pass's combat `care` offset. botLevel is the
// same pattern for §4.13's archetype eligibility filter (hs_archetype.h) —
// the worker thread draws the archetype and needs the bot's current level
// to restrict the draw to the eligible pool. rpgStatus is the same pattern
// again, new 2026-08-21: `botAI->rpgInfo.GetStatus()` (mod-playerbots'
// NewRpgStatus -- questing, grinding, outdoor PvP, resting, etc.) read at
// the call site and folded into the prompt as a short factual line, so a
// bot can't claim to be doing something the realm's own state contradicts
// (e.g. "pvping" while mid-quest) -- the same "never defended with a lie"
// concern §1/§4.20 already apply to mount/level/zone claims, extended to
// the bot's live activity.
// isFollowUp (new 2026-08-21, Claude/PLAN-engagement.md): true only for a
// self-initiated engagement follow-up (hs_engagement.cpp) -- same admission
// gates as any reply, but the worker skips the interaction-score bump and
// the history write for these (bot-initiated, not a scored player
// utterance -- same treatment openers already get; no history for the same
// reason the self-correction follow-up isn't written either -- not useful
// prior-turn context).
bool Hs_TryEnqueue(uint64_t botGuid, const std::string& botName, uint64_t senderGuid,
                    const std::string& senderName, bool isWhisper, const std::string& userPrompt,
                    bool inCombat, uint8_t botLevel, NewRpgStatus rpgStatus, bool isFollowUp);

// Delivers any replies the worker has finished since the last call. Must be
// called once per world tick, from the world thread only — this is the only
// place a Player*/PlayerbotAI* is ever touched for this subsystem, which is
// what fixes the data race in mod-ollama-chat's original pattern of calling
// botAI->Say() directly from a detached background thread (trap 3).
void Hs_DeliverPending();

// Drops any not-yet-delivered engagement follow-up (hs_engagement.h) queued
// for this player, across every bot -- called when they send a new message
// before a scheduled follow-up's deliverAt, so a stale one can't arrive
// after they've already said something else. Direct replies and the
// self-correction follow-up are never cancelled by this; only entries
// tagged isFollowUp are eligible.
void Hs_CancelPendingFollowUpsFor(uint64_t senderGuid);

// Delivers a tier-0 reflex reply (§3, step 9) -- and, since step 10, a
// grounded-answer reply too (§4.20): both are "answer without the GPU"
// paths that need the identical no-bucket, no-cooldown, no-worker-thread,
// no-history/identity-write delivery, so grounded answers reuse this
// function rather than duplicating it. No bucket, no cooldown, no worker
// thread, no history/identity write -- both callers only reach this after
// the arbiter selected the bot, and score nothing (§4.15: "tier 0 stays
// completely free of identity side effects"). Style-pass `text` before
// calling this (hs_style.h); this function delivers it verbatim after a
// short "a beat later" delay, reusing the same delivery-queue drain
// (Hs_DeliverPending) the self-correction follow-up already relies on
// rather than duplicating Say()/Whisper() dispatch. A call with empty
// `text` is a no-op (Silent BotQuestion mode, or the PersonalProbe pool's
// no-reply member -- matched, but nothing to deliver).
void Hs_DeliverReflexReply(uint64_t botGuid, uint64_t senderGuid, bool isWhisper, const std::string& text);

// Seconds since this bot's last *successfully delivered* reply, or
// UINT32_MAX if never. Read-only query for hs_arbiter's recent-speaker
// weighting; the hard cooldown gate itself lives in Hs_TryEnqueue.
uint32_t Hs_SecondsSinceLastReply(uint64_t botGuid);

// Read-only status for the `.hearthside status` GM command.
bool     Hs_IsBackendDown();
uint32_t Hs_PendingQueueDepth();

// PLAN.md §4.7's idle signal for the generator (step 12): "the module's own
// in-flight request count -- no need to poll /slots or NVML when llama.cpp
// has one [reactive] consumer." True only when the reactive worker has
// nothing queued and isn't mid-request -- the generator checks this before
// every generation call so it yields immediately rather than competing with
// a live reply for the GPU.
bool Hs_IsReactiveIdle();

// The most recent *pre-style* reply this bot sent (empty if none yet, or if
// the bot has sent one but never through the reactive tier). Backs
// `.hearthside capture` (§4.7): "taking the pre-style text" is the whole
// point -- the styled/typo'd version a player saw would bake this bot's
// specific misspelling into the corpus for every bot that ever draws the
// row (trap 12). Keyed by bot name since that's what a GM types; overwritten
// on every successful reactive reply, so "capture" always means "the last
// one," never an accumulating history.
std::string Hs_LastPreStyleReply(const std::string& botName);

#endif // MOD_HS_QUEUE_H
