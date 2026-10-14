#ifndef MOD_HS_SCRIPT_H
#define MOD_HS_SCRIPT_H

#include "ScriptMgr.h"
#include <cstdint>

// Scripted bot-to-bot conversations, replayed near a real player, never
// improvised (hand-written in base/hside_script_handwritten.sql; the
// generator can add more). A periodic scan (every kScanIntervalMs) looks for
// a real player with two settled, idle bots nearby, rolls a low chance,
// claims a script and schedules its turns with typing gaps. Each turn
// re-checks the abort conditions (the witness spoke, a participant left
// range, entered combat or died) just before it is sent, since minutes can
// pass between scheduling and the last turn. Its own small delivery
// mechanism: hs_queue.cpp's queue has no notion of a cancellable multi-turn
// run.
//
// Channel scripts (§4.17) share the file and the tick: 2 turns, no witness
// and no proximity abort (the whole channel is the audience), delivered with
// Channel::Say. Hs_ActiveScriptRunCount counts only the /say kind.

class HsScriptRunnerWorldScript : public WorldScript
{
public:
    HsScriptRunnerWorldScript() : WorldScript("HsScriptRunnerWorldScript") {}
    void OnUpdate(uint32_t diff) override;
};

// A pair of bots another module (City, through mod-hearthside-life) has
// stood together near a real player: queued, and on the next tick offered a
// /say script ahead of the regular scan. Any thread; it only queues.
void Hs_OfferScriptPair(uint64_t bot0Guid, uint64_t bot1Guid, uint64_t witnessGuid);

// Marks every currently active script run witnessed by this real player as
// aborted: a player speaking aborts the script, finishing the line in
// flight and stopping there. Called from hs_handler.cpp's real-player /say
// path only (bot-authored Say() calls, including a script's own turns,
// also pass through the chat hook but are filtered out before reaching
// this call).
void Hs_AbortScriptsWitnessedBy(uint64_t playerGuid);

// True while this bot is cast in a script run of either kind (/say or
// channel) that hasn't finished or aborted. Exposed for hs_ambient.cpp,
// which must not have a bot break its own scene by musing to itself
// mid-exchange: the two mechanisms share the same speakers and the same
// 30-second cadence, so without this they would collide regularly rather
// than rarely.
//
// Both run maps are checked because a bot cast in a channel scene is just as
// busy as one cast in a /say scene, even though only the latter counts
// toward Hs_ActiveScriptRunCount below.
bool Hs_IsBotInAnyScriptRun(uint64_t botGuid);

// Read-only status for `.hearthside status`: reserve depth already lives
// on hs_generator.h (Hs_ScriptReserveDepth); this is the runtime half.
uint32_t Hs_ActiveScriptRunCount();

// Scripts claimed (hside_script.consumed_at set) in the last 24h: reuses
// the column already written for claim-tracking rather than adding new
// instrumentation.
uint32_t Hs_ScriptsConsumedLast24h();

#endif // MOD_HS_SCRIPT_H
