#ifndef MOD_HS_OPENER_H
#define MOD_HS_OPENER_H

#include "ScriptMgr.h"
#include <cstdint>

// Openers are tier-1 corpus content a bot speaks first, fired off shared-
// context game events: group formed, mob killed jointly, rez given/
// received, dungeon completed, and prolonged proximity at a shared quest
// objective/flight master. The first four are one-shot event hooks; the
// fifth needs a periodic world-tick scan with duration tracking
// (Hs_ScanProximityOpeners below), ticked from hs_engagement.cpp's
// HsEngagementScanWorldScript, which already runs a periodic scan for its
// own reasons: one shared timer serves both rather than two near-
// identical ones.
//
// All five funnel into one shared Hs_FireOpener (hs_opener.cpp): the
// MaxTier.Openers ceiling check (corpus-only in v1), a per-bot-player
// cooldown and fire-chance roll (compiled constants, since opener trigger
// tuning is a live-realm judgement, not something an operator can
// meaningfully set yet), Hs_SelectOpenerLine (hs_corpus.h), the style
// pass, and delivery via the existing zero-GPU Hs_DeliverReflexReply path.
// No history/identity write and no score: bot-initiated openers must
// never increment interaction_score.

// Fires once per member added to a Group (GroupScript::OnAddMember);
// checks whether the group now mixes a bot and a real player and, if so,
// has the bot greet them.
class HsOpenerGroupHandler : public GroupScript
{
public:
    HsOpenerGroupHandler() : GroupScript("HsOpenerGroupHandler", { GROUPHOOK_ON_ADD_MEMBER }) {}
    void OnAddMember(Group* group, ObjectGuid guid) override;
};

// PlayerScript::OnPlayerCreatureKill fires once per player who lands the
// killing blow, not once per group member with kill credit, so this is
// scoped to the case where a bot lands the blow while grouped with a real
// player, rather than building a cross-player kill-credit correlation
// cache for a fuller "jointly" signal.
class HsOpenerKillHandler : public PlayerScript
{
public:
    HsOpenerKillHandler() : PlayerScript("HsOpenerKillHandler", { PLAYERHOOK_ON_CREATURE_KILL }) {}
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override;
};

// PlayerScript::OnPlayerResurrect only carries the receiver, not the
// caster: Player::ResurrectPlayer has several call sites and none pass a
// caster through. Rather than correlate a resurrect spell cast to this
// call within a time window, this is scoped to the receiving direction
// only: a bot was resurrected while grouped with a real player.
class HsOpenerResurrectHandler : public PlayerScript
{
public:
    HsOpenerResurrectHandler() : PlayerScript("HsOpenerResurrectHandler", { PLAYERHOOK_ON_PLAYER_RESURRECT }) {}
    void OnPlayerResurrect(Player* player, float restorePercent, bool& applySickness) override;
};

// GlobalScript::OnAfterUpdateEncounterState fires on every encounter-credit
// update in a map; dungeonCompleted is nonzero only when the credited
// encounter is the dungeon's last one. Any bot and real player still in
// the map at that moment are assumed to have shared the run, since normal
// dungeons/raids are group-locked, so this doesn't also require a shared
// Group.
class HsOpenerEncounterHandler : public GlobalScript
{
public:
    HsOpenerEncounterHandler() : GlobalScript("HsOpenerEncounterHandler", { GLOBALHOOK_ON_AFTER_UPDATE_ENCOUNTER_STATE }) {}
    void OnAfterUpdateEncounterState(Map* map, EncounterCreditType type, uint32_t creditEntry, Unit* source,
                                      Difficulty difficultyFixed, DungeonEncounterList const* encounters,
                                      uint32_t dungeonCompleted, bool updated) override;
};

// The fifth trigger: prolonged proximity at a shared quest objective/
// flight master. Not an event hook like the four above: called once per
// tick from hs_engagement.cpp's shared scan WorldScript. Walks online real
// players, finds nearby bots (reusing g_HsSayDistance as the proximity
// radius rather than a new config key), and once a (bot, player) pair has
// been observed within range continuously past a duration threshold, fires
// through the same FireOpener pipeline as the other four triggers.
void Hs_ScanProximityOpeners();

// Read-only status for the `.hearthside status` GM command: the only
// visibility this event-driven subsystem has when nothing is happening in
// a reachable game client.
uint32_t Hs_OpenersFiredThisSession();

#endif // MOD_HS_OPENER_H
