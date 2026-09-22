#ifndef MOD_HS_EVENT_H
#define MOD_HS_EVENT_H

#include "ScriptMgr.h"
#include <cstdint>

// Event triggers: bots reacting to things that *happen* rather than to
// things people say (Claude/archive/PLAN-ARBITER.md). Until this landed, the only
// unprompted bot speech was the five tier-1 corpus openers
// (hs_opener.cpp), which are canned lines fired off shared-context events.
// These are tier-2: a real generated reaction, routed through the same
// Hs_TryEnqueue admission as a direct reply.
//
// Each hook below gathers a candidate set, tags every candidate with how
// involved it is in what happened, and hands the set to
// Hs_ArbitrateEventReplies (hs_event_arbiter.h) to decide who, if anyone,
// speaks. All the pure weighting lives there; this file is the
// AzerothCore-facing half that turns live Player*s into that function's
// inputs and dispatches the result.
//
// None of the hooks does that work where it fires. They run on map-update
// threads, so each records GUIDs and strings and the gathering happens on
// the world thread: deaths through their own drain (the killer's name
// arrives on a second hook), everything else through Hs_DeferToWorldThread
// (hs_queue.h). A new hook here should do the same.
//
// Three properties worth knowing before adding a hook here:
//
// * **The combat gate does not apply.** g_HsDisableRepliesInCombat skips
//   in-combat bots on the /say path (hs_handler.cpp). A wipe means everyone
//   is in combat, so honouring it here would make death events fire almost
//   never: the feature would silently depend on an operator setting to
//   work at all. Event candidate sourcing is deliberately exempt
//   (Claude/archive/PLAN-ARBITER.md §3).
//
// * **Candidate sourcing differs by scope.** Party/raid-scoped events
//   (a death in a group, a groupmate's ding, a lost roll) structurally
//   require a real player present, since bots do not form groups on their
//   own on this realm, so no separate presence gate is needed. World-scoped
//   events (a solo death, a killing blow, a duel) source candidates by
//   say-range proximity and deliberately do *not* require a real player
//   nearby: bot-to-bot reaction is the same ambient texture the corpus
//   openers already provide.
//
// * **Events write no identity state.** Like openers and engagement
//   follow-ups, an event reaction is bot-initiated, so it appends no
//   history, bumps no interaction_score, and records no first meeting.
//   Hs_TryEnqueue's isEvent flag (hs_queue.h) is what enforces that.
//
// **This is not the only consumer of these hooks, and the distinction is the
// point.** hs_experience_store.h takes several of the same ones (deaths,
// dings) and hs_memory_store.h takes deaths too, but neither makes a bot
// speak: they record. Before adding an event type here, check that speech is
// actually what the event wants. Hs_EventCountBiasFor exists mostly to
// *suppress* reactions ("most deaths pass without comment"), so a new
// trigger whose right answer is usually silence belongs in hs_experience.h
// as background instead -- an event is often a backdrop rather than a topic,
// and an announcement per event is the tell that gives a bot away.

// Deaths, all four of Claude/archive/PLAN-ARBITER.md §5's death triggers off one hook.
// OnPlayerJustDied carries whichever Player* died, bot or real player, so
// branching on Hs_IsBot() inside covers both candidate sets without a second
// hook. HsMemoryDeathHandler (hs_memory_store.h) also registers this hook
// for its own "died together" memory beat; two PlayerScripts may both take
// it, and neither depends on the other's ordering.
//
// This hook no longer dispatches: it records the death, and the three
// classes below finish the job. See the "Deferred death dispatch" block in
// hs_event.cpp for why the killer's name forced that split.
class HsEventDeathHandler : public PlayerScript
{
public:
    HsEventDeathHandler() : PlayerScript("HsEventDeathHandler", { PLAYERHOOK_ON_PLAYER_JUST_DIED }) {}
    void OnPlayerJustDied(Player* player) override;
};

// The killer's name, which OnPlayerJustDied above cannot supply because
// Unit::Kill has not worked it out yet when that hook fires. Hooked as of
// 2026-09-07; hs_event.h used to document it as deliberately *not* hooked,
// on the grounds that the death prompt named no killer and so had nothing to
// feed. That was true while a stated proper noun was something a 1-3B model
// could only invent around. hs_rag.h changed it: "killed by Prince Taldaram"
// now retrieves Prince Taldaram's own authored paragraph on the way through
// hs_queue.cpp, so the name arrives grounded rather than bare.
//
// Fills in the pending record and nothing else -- it does not dispatch, and
// a death it never sees (a fall, a drowning, a PvP kill) still dispatches
// with no killer clause at all.
class HsEventKillerHandler : public PlayerScript
{
public:
    HsEventKillerHandler() : PlayerScript("HsEventKillerHandler", { PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE }) {}
    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override;
};

// Dispatches the deaths the two hooks above recorded, once per world update.
// Registered after both in hs_main.cpp -- not for correctness (World::Update
// runs the map, and so every hook, before OnWorldUpdate) but so the three
// halves of one mechanism read in order.
class HsEventDeathDrainWorldScript : public WorldScript
{
public:
    HsEventDeathDrainWorldScript() : WorldScript("HsEventDeathDrainWorldScript") {}
    void OnUpdate(uint32 diff) override;
};

// Dings. OnPlayerLevelChanged is the only after-the-fact level hook (there
// is no OnPlayerLevelIncreased variant) and it fires on any change, up or
// down, from one call site. The fire site guards on GetLevel() > oldlevel to
// skip a GM down-level; there is no way to tell an organic ding from a
// RandomPlayerbotMgr bracket relevel through this signature, and that noise
// is accepted (Claude/archive/PLAN-ARBITER.md §3).
class HsEventLevelHandler : public PlayerScript
{
public:
    HsEventLevelHandler() : PlayerScript("HsEventLevelHandler", { PLAYERHOOK_ON_LEVEL_CHANGED }) {}
    void OnPlayerLevelChanged(Player* player, uint8 oldlevel) override;
};

// A bot landing a killing blow on an enemy player. The reverse direction,
// a bot *killed by* an enemy player, was dropped deliberately: WotLK PvP
// is cross-faction and cross-faction players cannot read each other's chat,
// so the only audience for that line is the bot's own group, which the
// wipe/solo-death triggers already cover (Claude/archive/PLAN-ARBITER.md §5).
class HsEventPvpKillHandler : public PlayerScript
{
public:
    HsEventPvpKillHandler() : PlayerScript("HsEventPvpKillHandler", { PLAYERHOOK_ON_PVP_KILL }) {}
    void OnPlayerPVPKill(Player* killer, Player* killed) override;
};

// Group rolls. OnPlayerGroupRollRewardItem is preferred over OnPlayerLootItem
// for this pair because it carries both the item and the winner, where
// OnPlayerLootItem fires on every grey and would need a quality filter just
// to stop being noise. It still gets a rare-or-better filter here, since a
// group roll happens on greens too.
//
// The Roll* carries every participant's vote, which is what makes the
// "someone else won one the bot wanted" half possible: a bot that passed
// is not a candidate.
class HsEventRollHandler : public PlayerScript
{
public:
    HsEventRollHandler() : PlayerScript("HsEventRollHandler", { PLAYERHOOK_ON_GROUP_ROLL_REWARD_ITEM }) {}
    void OnPlayerGroupRollRewardItem(Player* player, Item* item, uint32 count, RollVote voteType, Roll* roll) override;
};

// Duels. Bots do accept player duel challenges on this realm, so both hooks
// actually fire. OnPlayerDuelEnd carries winner and loser in a single call,
// so it runs *one* arbitration over the combined pool and resolves which
// trigger text ("you just won" vs. "you just lost") only after selection
// picks a side (Claude/archive/PLAN-ARBITER.md §2).
class HsEventDuelHandler : public PlayerScript
{
public:
    HsEventDuelHandler() : PlayerScript("HsEventDuelHandler", {
        PLAYERHOOK_ON_DUEL_START,
        PLAYERHOOK_ON_DUEL_END,
    }) {}

    void OnPlayerDuelStart(Player* player1, Player* player2) override;
    void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override;
};

// Read-only status for the `.hearthside status` GM command, the same
// visibility problem hs_opener.h's counter solves, for the same reason:
// this subsystem is invisible when nothing is happening in a reachable
// game client.
uint32_t Hs_EventsFiredThisSession();

#endif // MOD_HS_EVENT_H
