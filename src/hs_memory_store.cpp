#include "hs_memory_store.h"
#include "hs_memory.h"
#include "hs_locale.h"

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

    // Review D1: event_type reaches these statements from module constants
    // today, but the surrounding statements escape every other
    // string-valued interpolation and these did not. Escaping it costs one
    // copy on a path that is already doing a MySQL round trip, and removes
    // the "inert today" qualifier entirely.
    std::string EscapedEventType(const std::string& eventType)
    {
        std::string escaped = eventType;
        CharacterDatabase.EscapeString(escaped);
        return escaped;
    }

    // Enforces the ~20-row cap (first_meeting exempt) by deleting the
    // oldest excess rows for this pair. MySQL's single-table DELETE
    // supports ORDER BY/LIMIT directly, so this is one statement rather
    // than a select-then-delete-by-id round trip.
    //
    // Review A4: the DELETE is DirectExecute so the COUNT(*) above is a
    // measurement of a settled table rather than a prediction made against
    // one with a previous eviction still queued on the async worker.
    void EvictOverflow(uint64_t botGuid, uint64_t playerGuid)
    {
        const std::string firstMeeting = EscapedEventType(kHsMemoryEventFirstMeeting);

        QueryResult countResult = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM hside_memory WHERE bot_guid = {} AND player_guid = {} AND event_type != '{}'",
            botGuid, playerGuid, firstMeeting);
        uint32_t count = countResult ? (*countResult)[0].Get<uint32_t>() : 0;
        if (count <= kHsMemoryRowCapPerPair)
            return;

        CharacterDatabase.DirectExecute(
            "DELETE FROM hside_memory WHERE bot_guid = {} AND player_guid = {} AND event_type != '{}' "
            "ORDER BY occurred_at ASC LIMIT {}",
            botGuid, playerGuid, firstMeeting, count - kHsMemoryRowCapPerPair);
    }

    // Review A3: DirectExecute. Both callers below are check-then-act
    // (SELECT ... LIMIT 1, then insert if absent) and hside_memory has no
    // unique constraint to fall back on -- only PRIMARY KEY (id) plus two
    // non-unique indexes -- so an async insert that has not drained leaves
    // the next check-then-act blind to it and both inserts land. Two bots
    // replying to one /say each reach Hs_EnsureFirstMeetingRecorded from the
    // queue worker; that is the concrete duplicate-first_meeting path.
    void InsertMemoryRow(uint64_t botGuid, uint64_t playerGuid, const std::string& eventType, const std::string& text)
    {
        std::string escapedText = text;
        CharacterDatabase.EscapeString(escapedText);
        CharacterDatabase.DirectExecute(
            "INSERT INTO hside_memory (bot_guid, player_guid, event_type, occurred_at, text, source) "
            "VALUES ({}, {}, '{}', NOW(), '{}', 'template')",
            botGuid, playerGuid, EscapedEventType(eventType), escapedText);
    }
}

void Hs_EnsureFirstMeetingRecorded(uint64_t botGuid, uint64_t playerGuid)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT 1 FROM hside_memory WHERE bot_guid = {} AND player_guid = {} AND event_type = '{}' LIMIT 1",
        botGuid, playerGuid, EscapedEventType(kHsMemoryEventFirstMeeting));
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
        botGuid, playerGuid, EscapedEventType(eventType), kHsMemoryDedupWindowSeconds);
    if (recent)
        return; // deduped: an identical beat already landed within the window

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
        botGuid, playerGuid, EscapedEventType(kHsMemoryEventDungeonCompleted));
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
        botGuid, playerGuid, EscapedEventType(kHsMemoryEventDungeonCompleted),
        EscapedEventType(kHsMemoryEventGroupedInZone));
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
        // "We went down together" has to be true of both sides. This hook
        // fires once per death, so the other half of the pair must already be
        // dead for the beat to be a fact rather than a claim the player
        // watched not happen (§4.13): a bot that pulls too much and dies
        // while its human groupmate is standing over the corpse must not
        // record a shared death. hs_event.cpp's wipe detection applies the
        // same IsAlive() test for the same reason. The effect is that the
        // beat fires on the *second* death of a pair, which is the correct
        // semantics; Hs_RecordMemoryEvent's 30-minute dedup window already
        // stops a full wipe writing one row per corpse.
        if (member->IsAlive())
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
    std::string zoneName = Hs_LocalizedAreaName(entry); // review H1
    std::string zone = zoneName.empty() ? "the field" : zoneName;

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
