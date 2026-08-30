#ifndef MOD_HS_MEMORY_H
#define MOD_HS_MEMORY_H

#include <cstdint>
#include <string>

// Memory and recall: pure logic only, no AzerothCore dependency, same
// standalone-testable shape as hs_archetype.cpp/hs_identity.cpp. Owns the
// event-type vocabulary and the clean, unstyled template text written into
// hside_memory.text. Style is a delivery-time concern, applied only when
// a recall answer is actually spoken, never baked into storage. The
// DB-touching half (hside_memory reads/writes, dedup, eviction, and the
// game hooks) lives in hs_memory_store.h, which calls into this file
// rather than duplicating its text.

// event_type vocabulary: first meeting plus four shared-experience beats.
// "Traded" is deliberately not built: this AzerothCore revision has no
// trade-completion hook, only pre-trade permission checks
// (OnPlayerCanInitTrade/OnPlayerCanSetTradeItem).
constexpr const char* kHsMemoryEventFirstMeeting     = "first_meeting";
constexpr const char* kHsMemoryEventDungeonCompleted = "dungeon_completed";
constexpr const char* kHsMemoryEventGroupedInZone    = "grouped_in_zone";
constexpr const char* kHsMemoryEventDiedTogether     = "died_together";
constexpr const char* kHsMemoryEventJoinedSameGuild  = "joined_same_guild";

// Deduped by event_type within this window, an authored constant rather
// than an operator knob. 30 minutes is long enough that a string of joint
// kills or re-grouping in the same zone doesn't spam a row per encounter,
// short enough that a real second visit still gets its own beat.
constexpr uint32_t kHsMemoryDedupWindowSeconds = 1800;

// Capped at ~20 rows per pair, evicting oldest first; the first-meeting row
// is exempt (pinned), matching the schema comment.
constexpr uint32_t kHsMemoryRowCapPerPair = 20;

// Clean, unstyled sentences: no per-bot voice, no typos, no archetype
// flavor. The style pass (hs_style.h) runs once, at recall delivery time in
// hs_handler.cpp, same as every other lookup-and-template source
// (hs_grounded.cpp, hs_corpus.cpp's card-placeholder resolution).
std::string Hs_BuildFirstMeetingText();
std::string Hs_BuildDungeonCompletedText(const std::string& dungeonOrRaidName);
std::string Hs_BuildGroupedInZoneText(const std::string& zoneName);
std::string Hs_BuildDiedTogetherText(const std::string& zoneName);
std::string Hs_BuildJoinedSameGuildText(const std::string& guildName);

#endif // MOD_HS_MEMORY_H
