#include "hs_memory_store.h"
#include "hs_memory.h"

#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Guild.h"
#include "Group.h"
#include "GroupReference.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "QueryResult.h"

namespace
{
    bool IsBot(Player* p)
    {
        if (!p)
            return false;
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(p);
        return ai && ai->IsBotAI();
    }

    // §4.12's ~20-row cap, first_meeting exempt (pinned) -- deletes the
    // oldest excess rows for this pair, if any. MySQL's single-table DELETE
    // supports ORDER BY/LIMIT directly, so this is one statement rather than
    // a select-then-delete-by-id round trip.
    void EvictOverflow(uint64_t botGuid, uint64_t playerGuid)
    {
        QueryResult countResult = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM hside_memory WHERE bot_guid = {} AND player_guid = {} AND event_type != '{}'",
            botGuid, playerGuid, kHsMemoryEventFirstMeeting);
        uint32_t count = countResult ? (*countResult)[0].Get<uint32_t>() : 0;
        if (count <= kHsMemoryRowCapPerPair)
            return;

        CharacterDatabase.Execute(
            "DELETE FROM hside_memory WHERE bot_guid = {} AND player_guid = {} AND event_type != '{}' "
            "ORDER BY occurred_at ASC LIMIT {}",
            botGuid, playerGuid, kHsMemoryEventFirstMeeting, count - kHsMemoryRowCapPerPair);
    }

    void InsertMemoryRow(uint64_t botGuid, uint64_t playerGuid, const std::string& eventType, const std::string& text)
    {
        std::string escapedText = text;
        CharacterDatabase.EscapeString(escapedText);
        CharacterDatabase.Execute(
            "INSERT INTO hside_memory (bot_guid, player_guid, event_type, occurred_at, text, source) "
            "VALUES ({}, {}, '{}', NOW(), '{}', 'template')",
            botGuid, playerGuid, eventType, escapedText);
    }
}

void Hs_EnsureFirstMeetingRecorded(uint64_t botGuid, uint64_t playerGuid)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT 1 FROM hside_memory WHERE bot_guid = {} AND player_guid = {} AND event_type = '{}' LIMIT 1",
        botGuid, playerGuid, kHsMemoryEventFirstMeeting);
    if (result)
        return;

    InsertMemoryRow(botGuid, playerGuid, kHsMemoryEventFirstMeeting, Hs_BuildFirstMeetingText());
}

void Hs_RecordMemoryEvent(uint64_t botGuid, uint64_t playerGuid, const std::string& eventType, const std::string& text)
{
    Hs_EnsureFirstMeetingRecorded(botGuid, playerGuid);

    QueryResult recent = CharacterDatabase.Query(
        "SELECT 1 FROM hside_memory WHERE bot_guid = {} AND player_guid = {} AND event_type = '{}' "
        "AND occurred_at >= NOW() - INTERVAL {} SECOND LIMIT 1",
        botGuid, playerGuid, eventType, kHsMemoryDedupWindowSeconds);
    if (recent)
        return; // deduped -- an identical beat already landed within the window

    InsertMemoryRow(botGuid, playerGuid, eventType, text);
    EvictOverflow(botGuid, playerGuid);
}

bool Hs_HasMetBefore(uint64_t botGuid, uint64_t playerGuid)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT 1 FROM hside_memory WHERE bot_guid = {} AND player_guid = {} LIMIT 1", botGuid, playerGuid);
    return result != nullptr;
}

HsMemoryFact Hs_LookupLastDungeonRun(uint64_t botGuid, uint64_t playerGuid)
{
    HsMemoryFact fact;
    QueryResult result = CharacterDatabase.Query(
        "SELECT text FROM hside_memory WHERE bot_guid = {} AND player_guid = {} AND event_type = '{}' "
        "ORDER BY occurred_at DESC LIMIT 1",
        botGuid, playerGuid, kHsMemoryEventDungeonCompleted);
    if (result)
    {
        fact.hasFact = true;
        fact.text    = (*result)[0].Get<std::string>();
    }
    return fact;
}

bool Hs_HasGroupedBefore(uint64_t botGuid, uint64_t playerGuid)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT 1 FROM hside_memory WHERE bot_guid = {} AND player_guid = {} AND event_type IN ('{}', '{}') LIMIT 1",
        botGuid, playerGuid, kHsMemoryEventDungeonCompleted, kHsMemoryEventGroupedInZone);
    return result != nullptr;
}

uint32_t Hs_MemoryRowCount()
{
    QueryResult result = CharacterDatabase.Query("SELECT COUNT(*) FROM hside_memory");
    return result ? (*result)[0].Get<uint32_t>() : 0;
}

void Hs_DropMemoryRowsForBot(uint64_t botGuid)
{
    CharacterDatabase.Execute("DELETE FROM hside_memory WHERE bot_guid = {}", botGuid);
}

void HsMemoryDeathHandler::OnPlayerJustDied(Player* player)
{
    if (!player || !player->IsInWorld())
        return;

    Group* group = player->GetGroup();
    if (!group)
        return;

    bool    diedIsBot = IsBot(player);
    Player* other      = nullptr;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || member == player || !member->IsInWorld())
            continue;
        if (IsBot(member) != diedIsBot)
        {
            other = member;
            break;
        }
    }
    if (!other)
        return;

    Player* bot        = diedIsBot ? player : other;
    Player* realPlayer = diedIsBot ? other : player;

    AreaTableEntry const* entry = sAreaTableStore.LookupEntry(player->GetZoneId());
    const char* zoneName = entry ? entry->area_name[0] : nullptr;
    std::string zone = (zoneName && *zoneName) ? zoneName : "the field";

    Hs_RecordMemoryEvent(bot->GetGUID().GetRawValue(), realPlayer->GetGUID().GetRawValue(),
                          kHsMemoryEventDiedTogether, Hs_BuildDiedTogetherText(zone));
}

void HsMemoryGuildHandler::OnAddMember(Guild* guild, Player* player, uint8& /*plRank*/)
{
    if (!guild || !player || !player->IsInWorld())
        return;

    bool    joinerIsBot = IsBot(player);
    Player* other        = nullptr;
    auto findOtherSide = [&](Player* member)
    {
        if (!other && member && member->IsInWorld() && IsBot(member) != joinerIsBot)
            other = member;
    };
    guild->BroadcastWorker(findOtherSide, player);
    if (!other)
        return;

    Player* bot        = joinerIsBot ? player : other;
    Player* realPlayer = joinerIsBot ? other : player;

    Hs_RecordMemoryEvent(bot->GetGUID().GetRawValue(), realPlayer->GetGUID().GetRawValue(),
                          kHsMemoryEventJoinedSameGuild, Hs_BuildJoinedSameGuildText(guild->GetName()));
}
