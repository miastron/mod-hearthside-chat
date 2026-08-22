#ifndef MOD_HS_MEMORY_STORE_H
#define MOD_HS_MEMORY_STORE_H

#include "ScriptMgr.h"

#include <cstdint>
#include <string>

// The DB-touching and game-hook half of memory and recall. Pure logic
// (event vocabulary, dedup/cap constants, template text) lives in
// hs_memory.h; this file owns hside_memory reads/writes and the
// PlayerScript/GuildScript hooks, same split as
// hs_gen_validate.h/hs_generator.h and hs_identity.h/hs_identity_store.h.

// Idempotent: inserts a first_meeting row for (botGuid, playerGuid) iff none
// exists yet. Called from every write path below (so a shared-experience
// event also seeds first-meeting if that's genuinely the pair's first
// contact) and directly from hs_queue.cpp's WorkerLoop on every tier-2
// delivery -- ordinary chat is by far the most common way two people
// actually meet.
void Hs_EnsureFirstMeetingRecorded(uint64_t botGuid, uint64_t playerGuid);

// Records one shared-experience beat: ensures first-meeting exists, skips
// if a row of this exact event_type for this exact pair already landed
// within kHsMemoryDedupWindowSeconds, otherwise inserts and evicts down to
// kHsMemoryRowCapPerPair (oldest first, first_meeting exempt). Safe to call
// from any thread; only touches CharacterDatabase.
void Hs_RecordMemoryEvent(uint64_t botGuid, uint64_t playerGuid, const std::string& eventType, const std::string& text);

// ---- recall lookups, consumed by hs_grounded.h's three Recall kinds ----

// "Do you remember me" -- any hside_memory row at all for this pair.
bool Hs_HasMetBefore(uint64_t botGuid, uint64_t playerGuid);

struct HsMemoryFact
{
    bool        hasFact = false;
    std::string text;
};

// "What did we run" -- the most recent dungeon_completed row's text.
HsMemoryFact Hs_LookupLastDungeonRun(uint64_t botGuid, uint64_t playerGuid);

// "Have we grouped before" -- any dungeon_completed or grouped_in_zone row.
bool Hs_HasGroupedBefore(uint64_t botGuid, uint64_t playerGuid);

// Read-only counter for `.hearthside status`.
uint32_t Hs_MemoryRowCount();

// Retirement: drops every row for every player this bot ever shared history
// with, unlike EvictOverflow's per-pair cap -- a retired bot is a different
// person going forward, so nothing it "remembers" is true anymore. Called
// from hs_identity_store.cpp's Hs_RetireCard.
void Hs_DropMemoryRowsForBot(uint64_t botGuid);

// "Died together": PlayerScript::OnPlayerJustDied carries whichever Player*
// just died -- unlike the rez opener's OnPlayerResurrect (receiver-only by
// hook limitation), this hook covers both directions for free, firing once
// per death regardless of which side of the bot/real-player pair died, so
// no separate "gave/received" scoping is needed here.
class HsMemoryDeathHandler : public PlayerScript
{
public:
    HsMemoryDeathHandler() : PlayerScript("HsMemoryDeathHandler", { PLAYERHOOK_ON_PLAYER_JUST_DIED }) {}
    void OnPlayerJustDied(Player* player) override;
};

// "Joined the same guild": GuildScript::OnAddMember fires once per member
// added, after the member is already in Guild's online-member set, so the
// guild's own member walk already sees the joiner. Finds "the other side"
// among currently online members rather than a full roster scan, same
// approach the group-formed opener uses.
class HsMemoryGuildHandler : public GuildScript
{
public:
    HsMemoryGuildHandler() : GuildScript("HsMemoryGuildHandler", { GUILDHOOK_ON_ADD_MEMBER }) {}
    void OnAddMember(Guild* guild, Player* player, uint8& plRank) override;
};

#endif // MOD_HS_MEMORY_STORE_H
