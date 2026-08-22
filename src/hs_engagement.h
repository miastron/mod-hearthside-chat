#ifndef MOD_HS_ENGAGEMENT_H
#define MOD_HS_ENGAGEMENT_H

#include "ScriptMgr.h"
#include <cstdint>

// Claude/PLAN-engagement.md (design draft, decisions resolved 2026-08-21): a
// bot continuing a conversation on its own initiative after it has already
// answered a player once -- a follow-up question or related comment, rather
// than going silent until the player speaks again. Generated (tier 2, a
// second Hs_CallLLM pass), gated by its own MaxTier.EngagementFollowUp
// ceiling, on both whisper and /say.
//
// One periodic scan WorldScript (HsEngagementScanWorldScript) serves two
// gaps: this feature, and hs_opener.h's own named-but-unbuilt fifth trigger
// ("prolonged proximity") -- see hs_opener.cpp's Hs_ScanProximityOpeners,
// called from the same OnUpdate tick rather than running a second
// near-identical timer.
//
// A follow-up chain is not capped at exactly one: each fired follow-up can
// earn another, but the fire chance decays per depth and the chain's real
// ceiling is the player -- it only continues if they keep replying (each
// direct reply re-arms eligibility via Hs_EngagementNoteDirectReply; a
// follow-up firing does not). A hard depth cap is a safety-valve backstop
// only, not the normal way a chain ends.

class HsEngagementScanWorldScript : public WorldScript
{
public:
    HsEngagementScanWorldScript() : WorldScript("HsEngagementScanWorldScript") {}
    void OnUpdate(uint32_t diff) override;
};

// Called from hs_queue.cpp's WorkerLoop after every successfully delivered
// *direct* reply (whisper or /say) -- never for a follow-up's own delivery.
// Marks this (bot, player) pair eligible for the scan to consider firing a
// follow-up once the fire window elapses; does not touch chain depth.
void Hs_EngagementNoteDirectReply(uint64_t botGuid, uint64_t senderGuid, bool isWhisper);

// Called from hs_handler.cpp's real-player /say and whisper paths -- same
// call site shape as hs_script.cpp's Hs_AbortScriptsWitnessedBy. Marks every
// tracked pair for this player ineligible until their next direct reply, and
// cancels any not-yet-delivered follow-up already in flight for them
// (hs_queue.h's Hs_CancelPendingFollowUpsFor) so a stale follow-up can't
// arrive after the player has already said something else.
void Hs_AbortEngagementFollowUpsFor(uint64_t playerGuid);

// Read-only status for the `.hearthside status` GM command.
uint32_t Hs_EngagementFollowUpsFiredThisSession();

#endif // MOD_HS_ENGAGEMENT_H
