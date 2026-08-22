#ifndef MOD_HS_MEMORY_H
#define MOD_HS_MEMORY_H

#include <cstdint>
#include <string>

// PLAN.md §4.12 / §7 step 16 -- memory and recall. Pure logic only, no
// AzerothCore dependency, same standalone-testable shape as
// hs_archetype.cpp/hs_identity.cpp: the event-type vocabulary and the clean,
// unstyled template text written into hside_memory.text (trap 12 -- style is
// a delivery-time concern, applied when a recall answer is actually spoken,
// never baked into storage). The DB-touching half (hside_memory reads/
// writes, dedup, eviction, and the two new game hooks this step adds) lives
// in hs_memory_store.h, which calls into this file rather than duplicating
// its text.

// event_type vocabulary -- exactly the five events PLAN.md names as "the
// events the interaction_score hooks already visit" (first meeting) plus
// the four new shared-experience beats this step adds hooks for. "Traded"
// is the sixth named event and is not built: this AzerothCore revision has
// no trade-completion hook at all, only pre-trade permission checks
// (OnPlayerCanInitTrade/OnPlayerCanSetTradeItem) -- confirmed by reading
// TradeHandler.cpp before starting, same "hard blocker, not a scope choice"
// class as step 14's channel-variant gap.
constexpr const char* kHsMemoryEventFirstMeeting     = "first_meeting";
constexpr const char* kHsMemoryEventDungeonCompleted = "dungeon_completed";
constexpr const char* kHsMemoryEventGroupedInZone    = "grouped_in_zone";
constexpr const char* kHsMemoryEventDiedTogether     = "died_together";
constexpr const char* kHsMemoryEventJoinedSameGuild  = "joined_same_guild";

// §4.12 "deduped by event_type within a window" -- an authored constant, not
// an operator knob, same class as the opener cooldown/fire-chance and the
// generator's dedup threshold: there is no number an operator has grounds to
// judge yet. 30 minutes is long enough that a string of joint kills or
// re-grouping in the same zone doesn't spam a row per encounter, short
// enough that a real second visit still gets its own beat.
constexpr uint32_t kHsMemoryDedupWindowSeconds = 1800;

// §4.12 "capped at ~20 rows per pair evicting oldest first" -- the
// first-meeting row is exempt (pinned), matching the schema comment.
constexpr uint32_t kHsMemoryRowCapPerPair = 20;

// Clean, unstyled sentences (trap 12) -- no per-bot voice, no typos, no
// archetype flavor. The style pass (hs_style.h) runs once, at recall
// delivery time in hs_handler.cpp, same as every other lookup-and-template
// source (hs_grounded.cpp, hs_corpus.cpp's card-placeholder resolution).
std::string Hs_BuildFirstMeetingText();
std::string Hs_BuildDungeonCompletedText(const std::string& dungeonOrRaidName);
std::string Hs_BuildGroupedInZoneText(const std::string& zoneName);
std::string Hs_BuildDiedTogetherText(const std::string& zoneName);
std::string Hs_BuildJoinedSameGuildText(const std::string& guildName);

#endif // MOD_HS_MEMORY_H
