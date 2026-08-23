#ifndef MOD_HS_SCRIPT_H
#define MOD_HS_SCRIPT_H

#include "ScriptMgr.h"
#include <cstdint>

// Scripted bot-to-bot conversations. Pre-generated during GPU idle
// (hs_generator.cpp's script-reserve queue), replayed near a real player --
// never improvised live (MaxTier.BotToBot stays corpus-only in v1). This is
// the module's first genuinely stateful runtime subsystem: elsewhere a
// reply is either one delivered line or a single fire-and-forget line. A
// script is several lines paced over time with something that can
// interrupt it partway through.
//
// Two moving parts:
// - A periodic proximity scan (HsScriptRunnerWorldScript::OnUpdate, gated to
//   roughly every kScanIntervalMs rather than every tick) that looks for a
//   real player with two idle, non-scripted bots nearby, rolls a low chance,
//   and if a script is available in the reserve, claims it and schedules its
//   turns with per-turn typing delays.
// - Per-turn delivery (also driven from the same OnUpdate) that re-checks
//   abort conditions -- the witness player spoke, or a participant left
//   range/combat/died -- immediately before sending each turn, not just at
//   schedule time, since minutes may pass between scheduling a script and
//   its last turn.
//
// Deliberately its own small delivery mechanism rather than reusing
// hs_queue.cpp's -- that queue is a single-reply-at-a-time subsystem with no
// concept of "this queued item belongs to a cancellable multi-turn run,"
// and retrofitting that would touch more of an already-tested file than
// building an independent, self-contained one here.
//
// §4.17 channel scripts are a parallel, smaller mechanism in this same
// file: 2 turns not 4, no proximity/combat abort (a channel cast needn't be
// co-located), no witness/interrupt concept (the whole channel is the
// audience, not one player), delivered via Channel::Say
// (Hs_ResolveChannelForDelivery, hs_queue.h) instead of PlayerbotAI::Say.
// Shares this file's WorldScript tick rather than running a second timer.
// Not reflected in Hs_ActiveScriptRunCount below (that stays scoped to the
// /say mechanism) -- a known small gap, not a functional one.

class HsScriptRunnerWorldScript : public WorldScript
{
public:
    HsScriptRunnerWorldScript() : WorldScript("HsScriptRunnerWorldScript") {}
    void OnUpdate(uint32_t diff) override;
};

// Marks every currently active script run witnessed by this real player as
// aborted -- a player speaking aborts the script, finishing the line in
// flight and stopping there. Called from hs_handler.cpp's real-player /say
// path only (bot-authored Say() calls, including a script's own turns,
// also pass through the chat hook but are filtered out before reaching
// this call).
void Hs_AbortScriptsWitnessedBy(uint64_t playerGuid);

// Read-only status for `.hearthside status` -- reserve depth already lives
// on hs_generator.h (Hs_ScriptReserveDepth); this is the runtime half.
uint32_t Hs_ActiveScriptRunCount();

// Scripts claimed (hside_script.consumed_at set) in the last 24h -- reuses
// the column already written for claim-tracking rather than adding new
// instrumentation.
uint32_t Hs_ScriptsConsumedLast24h();

#endif // MOD_HS_SCRIPT_H
