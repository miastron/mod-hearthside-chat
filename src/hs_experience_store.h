#ifndef MOD_HS_EXPERIENCE_STORE_H
#define MOD_HS_EXPERIENCE_STORE_H

#include "ScriptMgr.h"

// The game-hook half of ambient experience. The buffer, the collapsing, the
// decay and the renderer all live in hs_experience.h, which has no
// AzerothCore dependency; this file is only the hooks that feed it, same
// split as hs_memory.h/hs_memory_store.h.
//
// Unlike hs_memory_store.h there is no DB half at all -- see hs_experience.h
// on why "recently" is deliberately in-memory state.
//
// **Every hook here filters at the site, and that is the load-bearing part.**
// Four of these fire constantly: loot on every grey, money on every vendor
// sale and quest turn-in, skill on every swing of a pick, zone on every
// border crossing. Recording them all would fill a six-slot ring with noise
// inside a minute, and the ring's collapsing (hs_experience.cpp) only
// deduplicates repeats, it does not make an uninteresting event
// interesting. So each hook drops most of what it sees before recording:
// loot below a quality floor, money below a copper threshold, skill except
// on a milestone, zone except on an actual change. Three of those floors are
// operator-tunable (HearthsideChat.Experience.*, hs_config.h) because the
// right value depends on the realm's level spread and rates.
//
// Every hook also gates on Hs_IsBot: a human player's recent activity is
// never read by anything, so recording it would be pure retention.

// Quests, both directions. Completing one is the strongest single signal
// here (it is what a player was actually *doing*), and abandoning one is
// worth keeping because it carries a different mood about the same content.
//
// OnPlayerCompleteQuest carries the Quest*, OnPlayerQuestAbandon only the
// id, so the abandon side resolves the template itself before it can name
// anything. A quest id that no longer resolves records nothing rather than
// recording a number.
class HsExperienceQuestHandler : public PlayerScript
{
public:
    HsExperienceQuestHandler() : PlayerScript("HsExperienceQuestHandler", {
        PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
        PLAYERHOOK_ON_QUEST_ABANDON,
    }) {}

    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override;
    void OnPlayerQuestAbandon(Player* player, uint32 questId) override;
};

// Loot, quality-filtered. hs_event.h's roll handler already explains why
// OnPlayerLootItem is the noisy one of the two loot-shaped hooks: it fires
// on every grey. HearthsideChat.Experience.LootMinQuality (default 3, Rare)
// is the floor, and it is a config key rather than a constant because the
// interesting threshold moves with level -- a green is an event at 20 and
// vendor trash at 80.
class HsExperienceLootHandler : public PlayerScript
{
public:
    HsExperienceLootHandler() : PlayerScript("HsExperienceLootHandler", { PLAYERHOOK_ON_LOOT_ITEM }) {}
    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid lootguid) override;
};

// Money. The hook fires *before* the modification with the delta in
// `amount`, and it fires for every copper of vendor trash, so the threshold
// (HearthsideChat.Experience.MoneyMinCopper, default 10g) is what makes this
// hook affordable at all. Losses are ignored: `amount` is signed and only
// gains are recorded, since "spent 12 gold" is a purchase the bot has no
// other context for and would read as a non-sequitur in a prompt.
class HsExperienceMoneyHandler : public PlayerScript
{
public:
    HsExperienceMoneyHandler() : PlayerScript("HsExperienceMoneyHandler", { PLAYERHOOK_ON_MONEY_CHANGED }) {}
    void OnPlayerMoneyChanged(Player* player, int32& amount) override;
};

// Zone changes. Fires on every border crossing including sub-area churn, so
// this records only when the zone id actually differs from the last one
// recorded for that bot -- which the ring's own collapsing cannot do, since
// a bot walking A -> B -> A would otherwise keep both entries alive
// indefinitely.
class HsExperienceZoneHandler : public PlayerScript
{
public:
    HsExperienceZoneHandler() : PlayerScript("HsExperienceZoneHandler", { PLAYERHOOK_ON_UPDATE_ZONE }) {}
    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 newArea) override;
};

// Professions. Both hooks fire on every point gained, so neither is usable
// raw; each records only when the gain carries the skill across a multiple
// of HearthsideChat.Experience.SkillStep (default 25). That maps closely to
// how players actually talk about a profession -- nobody announces 287, and
// everyone announces 300.
//
// Two hooks rather than one because the core splits them and they carry
// different payloads: gathering hands over the skill id directly, crafting
// hands over a SkillLineAbilityEntry whose SkillLine field is that id.
// Fishing has its own hook too and is deliberately not taken: it shares
// OnPlayerUpdateGatheringSkill's shape but fires on a cadence that would
// crowd out everything else in the ring for a fishing bot.
class HsExperienceSkillHandler : public PlayerScript
{
public:
    HsExperienceSkillHandler() : PlayerScript("HsExperienceSkillHandler", {
        PLAYERHOOK_ON_UPDATE_GATHERING_SKILL,
        PLAYERHOOK_ON_UPDATE_CRAFTING_SKILL,
    }) {}

    void OnPlayerUpdateGatheringSkill(Player* player, uint32 skill_id, uint32 current, uint32 gray,
                                      uint32 green, uint32 yellow, uint32& gain) override;
    void OnPlayerUpdateCraftingSkill(Player* player, SkillLineAbilityEntry const* skill,
                                     uint32 current_level, uint32& gain) override;
};

// Dings. A third PlayerScript on PLAYERHOOK_ON_LEVEL_CHANGED alongside
// HsEventLevelHandler (hs_event.h); several scripts may take one hook and
// none depends on another's ordering, the same arrangement the two death
// handlers already have.
//
// Guards on GetLevel() > oldlevel for the same reason hs_event.cpp does: the
// hook fires on any change and a GM down-level or a RandomPlayerbotMgr
// bracket relevel is not a ding.
class HsExperienceLevelHandler : public PlayerScript
{
public:
    HsExperienceLevelHandler() : PlayerScript("HsExperienceLevelHandler", { PLAYERHOOK_ON_LEVEL_CHANGED }) {}
    void OnPlayerLevelChanged(Player* player, uint8 oldlevel) override;
};

// Deaths, recorded against the zone they happened in. The third script on
// PLAYERHOOK_ON_PLAYER_JUST_DIED, after HsMemoryDeathHandler and
// HsEventDeathHandler.
//
// This is the entry the ring's collapsing was really built for: repeated
// deaths in one place fold into a single "died in Sholazar Basin (4 times)",
// which is both the noise fix and the only wipe-shaped signal the module has.
// Duels and group membership, added 2026-09-20.
//
// hs_event.cpp already hooks both duel ends, but it hooks them to *speak*:
// FireEvent produces a proactive line at the moment the duel resolves. That
// is a different job from this one. Nothing recorded the outcome where a
// later *reply* could read it, and reply prompts read this ring
// (hs_queue.cpp's Hs_ExperienceContext call), so a bot that had just lost a
// duel answered "bwahahaha" with "ur not even trying" and "you're terrible"
// with "you're not helping" -- a winner's taunt and a group-content framing,
// from a bot that had just lost a duel (realm 2026-09-20).
//
// Two PlayerScripts already share PLAYERHOOK_ON_PLAYER_JUST_DIED, so a third
// script on a hook hs_event.cpp also takes is the established shape here, not
// a new one.
class HsExperienceDuelHandler : public PlayerScript
{
public:
    HsExperienceDuelHandler() : PlayerScript("HsExperienceDuelHandler", { PLAYERHOOK_ON_DUEL_END }) {}
    void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override;
};

// Group joins arrive on GroupScript, not PlayerScript: PLAYERHOOK_CAN_GROUP_ACCEPT
// is a gate that fires before the join and can veto it, which is the wrong
// moment and the wrong contract for recording that a join happened.
// GROUPHOOK_ON_ADD_MEMBER fires after the fact and carries the member's guid.
//
// This is the module's only GroupScript; it is registered in hs_main.cpp
// alongside the PlayerScripts.
class HsExperienceGroupHandler : public GroupScript
{
public:
    HsExperienceGroupHandler() : GroupScript("HsExperienceGroupHandler", { GROUPHOOK_ON_ADD_MEMBER }) {}
    void OnAddMember(Group* group, ObjectGuid guid) override;
};

class HsExperienceDeathHandler : public PlayerScript
{
public:
    HsExperienceDeathHandler() : PlayerScript("HsExperienceDeathHandler", { PLAYERHOOK_ON_PLAYER_JUST_DIED }) {}
    void OnPlayerJustDied(Player* player) override;
};

#endif // MOD_HS_EXPERIENCE_STORE_H
