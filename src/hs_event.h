#ifndef MOD_HS_EVENT_H
#define MOD_HS_EVENT_H

#include "ScriptMgr.h"
#include <cstdint>
#include <string>

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

// ---- 2026-09-23: fifteen more event types, one retrain ----------------------
//
// Added together, with the fine-tune rows for every one of them, so the
// vocabulary grows once per training run rather than once per event
// (HsEventType in hs_event_arbiter.h has the list). Every trigger string
// below is frozen the same way the older ones are: the dataset copies them
// verbatim (Claude/finetune/matrix/event.txt), and a reworded trigger is a
// prompt the model was never trained on.
//
// The social ones -- a guild login, a new guild member, a guildmate's ding or
// achievement -- are real players only; see FireGuildEvent in hs_event.cpp.

// The bot has just joined a group a real player leads: "You have just joined
// Themaster's group." Replaces the bot-joins half of hs_opener.cpp's
// opener_group_formed, which now stands aside for it.
class HsEventGroupJoinHandler : public GroupScript
{
public:
    HsEventGroupJoinHandler() : GroupScript("HsEventGroupJoinHandler", { GROUPHOOK_ON_ADD_MEMBER }) {}
    void OnAddMember(Group* group, ObjectGuid guid) override;
};

// A dungeon or raid boss down: "Your group has just killed Edwin VanCleef."
// Read off the encounter credit rather than the creature kill, because that
// is what knows an encounter was newly completed and what names it (a boss
// credited by spell has no dying creature). On the last boss the
// dungeon-complete opener speaks and this only records the follow-up fact.
class HsEventEncounterHandler : public GlobalScript
{
public:
    HsEventEncounterHandler() : GlobalScript("HsEventEncounterHandler", { GLOBALHOOK_ON_AFTER_UPDATE_ENCOUNTER_STATE }) {}
    void OnAfterUpdateEncounterState(Map* map, EncounterCreditType type, uint32_t creditEntry, Unit* source,
                                     Difficulty difficultyFixed, DungeonEncounterList const* encounters,
                                     uint32_t dungeonCompleted, bool updated) override;
};

// A real player has just resurrected the bot: "Themaster has just resurrected
// you." OnPlayerResurrect carries no caster; Hs_RealPlayerResurrecting below
// recovers it. Replaces hs_opener.cpp's opener_rez whenever it finds one.
class HsEventResurrectHandler : public PlayerScript
{
public:
    HsEventResurrectHandler() : PlayerScript("HsEventResurrectHandler", { PLAYERHOOK_ON_PLAYER_RESURRECT }) {}
    void OnPlayerResurrect(Player* player, float restorePercent, bool& applySickness) override;
};

// Trades between a bot and a real player: the window opening ("Themaster has
// just opened a trade with you.") and the trade going through ("Themaster
// has just given you 5 gold." / "You have just given Themaster Linen
// Cloth."). There is no trade-complete hook; see the "Trade completion"
// block in hs_event.cpp for how the two hooks below stand in for one.
class HsEventTradeHandler : public PlayerScript
{
public:
    HsEventTradeHandler() : PlayerScript("HsEventTradeHandler", {
        PLAYERHOOK_CAN_INIT_TRADE,
        PLAYERHOOK_ON_MONEY_CHANGED,
        PLAYERHOOK_ON_AFTER_MOVE_ITEM_TO_INVENTORY,
    }) {}

    bool OnPlayerCanInitTrade(Player* player, Player* target) override;
    void OnPlayerMoneyChanged(Player* player, int32& amount) override;
    void OnPlayerAfterMoveItemToInventory(Player* player, Item* item, bool update) override;
};

// A real player in the bot's guild logging in: "Themaster, in your guild, has
// just logged in."
class HsEventLoginHandler : public PlayerScript
{
public:
    HsEventLoginHandler() : PlayerScript("HsEventLoginHandler", { PLAYERHOOK_ON_LOGIN }) {}
    void OnPlayerLogin(Player* player) override;
};

// Achievements: a real guildmate's ("Themaster, in your guild, has just
// earned the achievement X.") and the bot's own ("You have just earned the
// achievement X."). Statistics (ACHIEVEMENT_FLAG_COUNTER) are skipped.
class HsEventAchievementHandler : public PlayerScript
{
public:
    HsEventAchievementHandler() : PlayerScript("HsEventAchievementHandler", { PLAYERHOOK_ON_ACHI_COMPLETE }) {}
    void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override;
};

// A real player joining, leaving, or being removed from the bot's guild.
class HsEventGuildHandler : public GuildScript
{
public:
    HsEventGuildHandler() : GuildScript("HsEventGuildHandler", {
        GUILDHOOK_ON_ADD_MEMBER,
        GUILDHOOK_ON_REMOVE_MEMBER,
    }) {}

    void OnAddMember(Guild* guild, Player* player, uint8& plRank) override;
    void OnRemoveMember(Guild* guild, Player* player, bool isDisbanding, bool isKicked) override;
};

// Battleground and arena results, per side: "Your side has just won Warsong
// Gulch." / "Your team has just lost a 2v2 arena match." Only a side with a
// real player on it gets anything.
class HsEventBattlegroundHandler : public AllBattlegroundScript
{
public:
    HsEventBattlegroundHandler() : AllBattlegroundScript("HsEventBattlegroundHandler", { ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_END }) {}
    void OnBattlegroundEnd(Battleground* bg, TeamId winnerTeamId) override;
};

// The real player whose resurrect spell this bot is accepting, or nullptr (a
// spirit healer, a soulstone, a bot priest). Call from the resurrect hook
// itself, on the bot's map thread: it walks that map's player list. Shared
// with hs_opener.cpp so its opener_rez can stand aside when this event covers
// the moment.
Player* Hs_RealPlayerResurrecting(Player* bot);

// The follow-up half of every event: the state line of the most recent event
// this bot was part of, if it is at most two minutes old, for any reply it
// makes that is not itself an event reaction -- plus "The one talking to you
// now is X." when `senderGuid` is the other person in that event. "" when
// there is none. hs_queue.cpp's WorkerLoop appends it last in the system
// turn, the slot an event reaction's own state line takes. Safe from any
// thread (the reactive worker reads it while hooks write it).
std::string Hs_RecentEventContext(uint64_t botGuid, uint64_t senderGuid);

// Read-only status for the `.hearthside status` GM command, the same
// visibility problem hs_opener.h's counter solves, for the same reason:
// this subsystem is invisible when nothing is happening in a reachable
// game client.
uint32_t Hs_EventsFiredThisSession();

#endif // MOD_HS_EVENT_H
