#ifndef MOD_HS_OPENER_H
#define MOD_HS_OPENER_H

#include "ScriptMgr.h"
#include <cstdint>

// PLAN.md §3/§7 step 13: openers are tier-1 corpus content a bot speaks
// first, fired off shared-context game events -- "unprompted questions to
// passing strangers read as spam; real players open at specific moments,
// all of which are already hooked events." §3 names five: group formed,
// mob killed jointly, rez given/received, dungeon completed, and prolonged
// proximity at a shared quest objective/flight master. The first slice
// covered the four one-shot game-event triggers; the fifth needed a
// periodic world-tick scan with duration tracking this module didn't have.
// Built 2026-08-21 (Claude/PLAN-engagement.md): Hs_ScanProximityOpeners
// below, ticked from hs_engagement.cpp's HsEngagementScanWorldScript --
// that feature needed the same kind of periodic scan for its own reasons,
// so one shared timer serves both rather than two near-identical ones.
//
// All four hooks funnel into one shared Hs_FireOpener (hs_opener.cpp): the
// MaxTier.Openers ceiling check (§4.14, corpus-only in v1), a per-bot-player
// cooldown and fire-chance roll (compiled constants -- §6 lists "opener
// trigger tuning" as a live-realm-only judgement, so there's no config key
// an operator could meaningfully set yet), Hs_SelectOpenerLine (hs_corpus.h),
// the style pass, and delivery via the existing zero-GPU
// Hs_DeliverReflexReply path. No history/identity write and no score --
// tier 0's "completely free of identity side effects" (§4.15) applies here
// too, and matters more here: §4.12 explicitly bars bot-initiated openers
// from ever incrementing interaction_score once step 15 builds it, so
// nothing in this file may touch that column when it exists.

// "A group just formed" (§3). Fires once per member added to a Group
// (GroupScript::OnAddMember); checks whether the group now mixes a bot and
// a real player and, if so, has the bot greet them.
class HsOpenerGroupHandler : public GroupScript
{
public:
    HsOpenerGroupHandler() : GroupScript("HsOpenerGroupHandler", { GROUPHOOK_ON_ADD_MEMBER }) {}
    void OnAddMember(Group* group, ObjectGuid guid) override;
};

// "A mob was killed jointly" (§3). PlayerScript::OnPlayerCreatureKill fires
// once per player who lands the killing blow -- not once per group member
// with kill credit -- so this is scoped to the case where a *bot* lands the
// blow while grouped with a real player, rather than building a
// cross-player kill-credit correlation cache for a fuller "jointly" signal
// (§7 step 13 scoping decision).
class HsOpenerKillHandler : public PlayerScript
{
public:
    HsOpenerKillHandler() : PlayerScript("HsOpenerKillHandler", { PLAYERHOOK_ON_CREATURE_KILL }) {}
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override;
};

// "A rez was given or received" (§3). PlayerScript::OnPlayerResurrect only
// carries the receiver, not the caster -- Player::ResurrectPlayer has
// several call sites and none pass a caster through. Rather than correlate
// a resurrect spell cast to this call within a time window, this is scoped
// to the receiving direction only: a *bot* was resurrected while grouped
// with a real player (§7 step 13 scoping decision).
class HsOpenerResurrectHandler : public PlayerScript
{
public:
    HsOpenerResurrectHandler() : PlayerScript("HsOpenerResurrectHandler", { PLAYERHOOK_ON_PLAYER_RESURRECT }) {}
    void OnPlayerResurrect(Player* player, float restorePercent, bool& applySickness) override;
};

// "A dungeon just completed" (§3). GlobalScript::OnAfterUpdateEncounterState
// fires on every encounter-credit update in a map; dungeonCompleted is
// nonzero only when the credited encounter is the dungeon's last one
// (confirmed at the Map::UpdateEncounterState call site). Any bot and real
// player still in the map at that moment are assumed to have shared the
// run -- normal dungeons/raids are group-locked, so this doesn't also
// require a shared Group.
class HsOpenerEncounterHandler : public GlobalScript
{
public:
    HsOpenerEncounterHandler() : GlobalScript("HsOpenerEncounterHandler", { GLOBALHOOK_ON_AFTER_UPDATE_ENCOUNTER_STATE }) {}
    void OnAfterUpdateEncounterState(Map* map, EncounterCreditType type, uint32_t creditEntry, Unit* source,
                                      Difficulty difficultyFixed, DungeonEncounterList const* encounters,
                                      uint32_t dungeonCompleted, bool updated) override;
};

// "Prolonged proximity at a shared quest objective/flight master" (§3), the
// fifth trigger. Not an event hook like the four above -- called once per
// tick from hs_engagement.cpp's shared scan WorldScript. Walks online real
// players, finds nearby bots (reusing g_HsSayDistance as the proximity
// radius rather than a new config key -- same "compiled constant, live-
// realm tuning only" reasoning as kOpenerCooldownSeconds/
// kOpenerFireChancePercent above), and once a (bot, player) pair has been
// observed within range continuously past a duration threshold, fires
// through the same FireOpener pipeline as the other four triggers.
void Hs_ScanProximityOpeners();

// Read-only status for the `.hearthside status` GM command -- the only
// visibility this event-driven subsystem has when nothing is happening in
// a reachable game client (same shape as the generator's own session
// counter, hs_generator.h).
uint32_t Hs_OpenersFiredThisSession();

#endif // MOD_HS_OPENER_H
