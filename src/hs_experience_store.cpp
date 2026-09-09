#include "hs_experience_store.h"
#include "hs_bot.h"
#include "hs_config.h"
#include "hs_experience.h"
#include "hs_locale.h"

#include "DBCStores.h"
#include "DBCStructure.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"

#include <string>
#include <unordered_map>
#include <mutex>

namespace
{
    // The zone a player is standing in, resolved the same way every other
    // zone read in this module does (review H1: nothing indexes a DBC name
    // array directly). Empty when the area has no AreaTableEntry, which the
    // callers treat as "nothing true to record" rather than substituting a
    // placeholder -- same rule as hs_corpus.h's resolvers.
    std::string ZoneNameOf(Player* player)
    {
        AreaTableEntry const* entry = sAreaTableStore.LookupEntry(player->GetZoneId());
        return Hs_LocalizedAreaName(entry); // review H1
    }

    // Last zone id recorded per bot, so a border crossing that does not
    // change the zone records nothing.
    //
    // This cannot be done by the ring's own collapsing: a bot walking
    // A -> B -> A would find no matching entry on its return to A (B was
    // recorded in between), push a duplicate, and keep both alive. The ring
    // dedupes *repeats*; only a remembered previous value dedupes a
    // round trip.
    //
    // Bounded by the realm's bot population and holding nothing but an id,
    // so it is deliberately not pruned -- the same reasoning hs_prune.h
    // gives for the maps it does not cover.
    std::mutex                              g_LastZoneMutex;
    std::unordered_map<uint64_t, uint32_t>  g_LastZoneByBot;

    bool ZoneChanged(uint64_t botGuid, uint32_t newZone)
    {
        std::lock_guard<std::mutex> lock(g_LastZoneMutex);
        auto it = g_LastZoneByBot.find(botGuid);
        if (it != g_LastZoneByBot.end() && it->second == newZone)
            return false;
        g_LastZoneByBot[botGuid] = newZone;
        return true;
    }

    // True when `gain` carries `current` across a multiple of `step`, i.e.
    // the skill just passed a round number worth mentioning. Returns the
    // milestone itself so the caller names 300 rather than the 301 the
    // player actually landed on.
    //
    // A step of 0 disables the filter's arithmetic entirely rather than
    // dividing by zero: the operator set it to "record nothing", so nothing
    // is recorded.
    bool SkillMilestone(uint32_t current, uint32_t gain, uint32_t step, uint32_t& milestone)
    {
        if (step == 0 || gain == 0)
            return false;

        uint32_t after = current + gain;
        if (after / step == current / step)
            return false;

        milestone = (after / step) * step;
        return milestone != 0;
    }
}

void HsExperienceQuestHandler::OnPlayerCompleteQuest(Player* player, Quest const* quest)
{
    if (!g_HsExperienceEnable || !Hs_IsBot(player) || !quest)
        return;

    Hs_RecordExperience(player->GetGUID().GetRawValue(), HsExperienceKind::QuestCompleted,
                        Hs_LocalizedQuestTitle(quest));
}

void HsExperienceQuestHandler::OnPlayerQuestAbandon(Player* player, uint32 questId)
{
    if (!g_HsExperienceEnable || !Hs_IsBot(player))
        return;

    // Only the id arrives here, so the template has to be resolved before
    // anything can be named. An id that no longer resolves records nothing.
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return;

    Hs_RecordExperience(player->GetGUID().GetRawValue(), HsExperienceKind::QuestAbandoned,
                        Hs_LocalizedQuestTitle(quest));
}

void HsExperienceLootHandler::OnPlayerLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid /*lootguid*/)
{
    if (!g_HsExperienceEnable || !Hs_IsBot(player) || !item)
        return;

    ItemTemplate const* tmpl = item->GetTemplate();
    if (!tmpl || tmpl->Quality < g_HsExperienceLootMinQuality)
        return;

    Hs_RecordExperience(player->GetGUID().GetRawValue(), HsExperienceKind::LootedItem,
                        Hs_LocalizedItemName(tmpl));
}

void HsExperienceMoneyHandler::OnPlayerMoneyChanged(Player* player, int32& amount)
{
    if (!g_HsExperienceEnable || !Hs_IsBot(player))
        return;

    // Gains only, and only above the threshold. `amount` is the signed delta
    // and the hook fires before the modification lands, so this reads it
    // without touching the player's actual money at all.
    if (amount <= 0 || static_cast<uint32_t>(amount) < g_HsExperienceMoneyMinCopper)
        return;

    uint32_t gold = static_cast<uint32_t>(amount) / 10000u;
    if (gold == 0)
        return; // threshold below a gold piece: nothing round to say

    Hs_RecordExperience(player->GetGUID().GetRawValue(), HsExperienceKind::MoneyGained,
                        std::to_string(gold));
}

void HsExperienceZoneHandler::OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 /*newArea*/)
{
    if (!g_HsExperienceEnable || !Hs_IsBot(player))
        return;

    if (!ZoneChanged(player->GetGUID().GetRawValue(), newZone))
        return;

    AreaTableEntry const* entry = sAreaTableStore.LookupEntry(newZone);
    Hs_RecordExperience(player->GetGUID().GetRawValue(), HsExperienceKind::ZoneEntered,
                        Hs_LocalizedAreaName(entry)); // review H1
}

void HsExperienceSkillHandler::OnPlayerUpdateGatheringSkill(Player* player, uint32 skill_id, uint32 current,
                                                            uint32 /*gray*/, uint32 /*green*/, uint32 /*yellow*/,
                                                            uint32& gain)
{
    if (!g_HsExperienceEnable || !Hs_IsBot(player))
        return;

    uint32_t milestone = 0;
    if (!SkillMilestone(current, gain, g_HsExperienceSkillStep, milestone))
        return;

    std::string skillName = Hs_LocalizedSkillName(skill_id);
    if (skillName.empty())
        return;

    Hs_RecordExperience(player->GetGUID().GetRawValue(), HsExperienceKind::SkillUp,
                        std::to_string(milestone) + " " + skillName);
}

void HsExperienceSkillHandler::OnPlayerUpdateCraftingSkill(Player* player, SkillLineAbilityEntry const* skill,
                                                            uint32 current_level, uint32& gain)
{
    if (!g_HsExperienceEnable || !Hs_IsBot(player) || !skill)
        return;

    uint32_t milestone = 0;
    if (!SkillMilestone(current_level, gain, g_HsExperienceSkillStep, milestone))
        return;

    std::string skillName = Hs_LocalizedSkillName(skill->SkillLine);
    if (skillName.empty())
        return;

    Hs_RecordExperience(player->GetGUID().GetRawValue(), HsExperienceKind::SkillUp,
                        std::to_string(milestone) + " " + skillName);
}

void HsExperienceLevelHandler::OnPlayerLevelChanged(Player* player, uint8 oldlevel)
{
    if (!g_HsExperienceEnable || !Hs_IsBot(player))
        return;

    // Same guard as hs_event.cpp's level fire site: the hook fires on any
    // change, and a down-level is not a ding.
    if (player->GetLevel() <= oldlevel)
        return;

    Hs_RecordExperience(player->GetGUID().GetRawValue(), HsExperienceKind::LevelUp,
                        std::to_string(player->GetLevel()));
}

void HsExperienceDeathHandler::OnPlayerJustDied(Player* player)
{
    if (!g_HsExperienceEnable || !Hs_IsBot(player))
        return;

    Hs_RecordExperience(player->GetGUID().GetRawValue(), HsExperienceKind::Died, ZoneNameOf(player));
}
