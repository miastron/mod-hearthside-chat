#include "hs_locale.h"

#include "DBCStores.h"
#include "DBCStructure.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include "SpellInfo.h"
#include "World.h"

namespace
{
    // The realm's configured DBC locale, as an index into the fixed-size
    // name arrays DBC entries carry.
    uint8 DbcLocale()
    {
        return sWorld->GetDefaultDbcLocale();
    }
}

std::string Hs_LocalizedAreaName(AreaTableEntry const* entry)
{
    if (!entry)
        return "";

    // Byte-for-byte what PlayerbotAI::GetLocalizedAreaName does. Duplicated
    // rather than called so this file needs no mod-playerbots include, and
    // so the three helpers here share one visible rule.
    std::string name = entry->area_name[DbcLocale()];
    if (name.empty())
        name = entry->area_name[LOCALE_enUS];
    return name;
}

std::string Hs_LocalizedItemName(ItemTemplate const* tmpl)
{
    if (!tmpl)
        return "";

    // ItemTemplate::Name1 is the enUS string from item_template; the
    // localized override lives in locale_item and is keyed by item id. Same
    // two-step the core itself uses when it builds an item hyperlink
    // (PlayerStorage.cpp). GetLocaleString leaves `name` untouched when the
    // locale row is missing or its entry is empty, so the enUS fallback is
    // automatic.
    std::string name = tmpl->Name1;
    if (ItemLocale const* locale = sObjectMgr->GetItemLocale(tmpl->ItemId))
        ObjectMgr::GetLocaleString(locale->Name, static_cast<int>(DbcLocale()), name);
    return name;
}

std::string Hs_LocalizedSpellName(SpellInfo const* info)
{
    if (!info)
        return "";

    char const* name = info->SpellName[DbcLocale()];
    if (!name || !*name)
        name = info->SpellName[LOCALE_enUS];
    return (name && *name) ? std::string(name) : std::string();
}

std::string Hs_LocalizedQuestTitle(Quest const* quest)
{
    if (!quest)
        return "";

    // Same two-step as Hs_LocalizedItemName: the template carries the enUS
    // string and locale_quest carries the override, keyed by quest id.
    std::string title = quest->GetTitle();
    if (QuestLocale const* locale = sObjectMgr->GetQuestLocale(quest->GetQuestId()))
        ObjectMgr::GetLocaleString(locale->Title, static_cast<int>(DbcLocale()), title);
    return title;
}

std::string Hs_LocalizedSkillName(uint32_t skillId)
{
    SkillLineEntry const* entry = sSkillLineStore.LookupEntry(skillId);
    if (!entry)
        return "";

    char const* name = entry->name[DbcLocale()];
    if (!name || !*name)
        name = entry->name[LOCALE_enUS];
    return (name && *name) ? std::string(name) : std::string();
}

std::string Hs_LocalizedAchievementName(AchievementEntry const* entry)
{
    if (!entry)
        return "";

    char const* name = entry->name[DbcLocale()];
    if (!name || !*name)
        name = entry->name[LOCALE_enUS];
    return (name && *name) ? std::string(name) : std::string();
}

std::string Hs_LocalizedEncounterName(DungeonEncounterEntry const* entry)
{
    if (!entry)
        return "";

    char const* name = entry->encounterName[DbcLocale()];
    if (!name || !*name)
        name = entry->encounterName[LOCALE_enUS];
    return (name && *name) ? std::string(name) : std::string();
}
