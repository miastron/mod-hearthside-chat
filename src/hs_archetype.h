#ifndef MOD_HS_ARCHETYPE_H
#define MOD_HS_ARCHETYPE_H

#include <array>
#include <cstdint>
#include <string>

// PLAN.md §4.11 "The archetype enum" / §7 step 7, extended 2026-08-21 to
// load its content from SQL (hside_archetype) instead of a compiled table --
// closes the gap between §4.11's own text ("weight is per-archetype config
// so it can be retuned without touching bots") and the code, which had never
// actually made it configurable. The archetype *enum* stays fixed in code
// (ring-eligibility logic and Hs_ArchetypeInfoFor's index-by-enum-value
// depend on a stable ordinal), but every tunable column is now data.
//
// Split identically to hs_identity.h/hs_identity_store.h: this file is pure
// logic (the in-memory table plus the weighted draw), no AzerothCore
// dependency, standalone-testable. hs_archetype_store.h/.cpp owns the
// hside_archetype query and calls Hs_SetArchetypeTable once at startup.
//
// Declaration order here must match kEnumNames' order in hs_archetype.cpp --
// Hs_ArchetypeInfoFor indexes the in-memory table directly by this enum's
// underlying value rather than switching on it.
enum class HsArchetype
{
    RaiderSerious,
    RaiderCasual,
    PvpSerious,
    PvpCasual,
    Trader,
    Lootgoblin,
    Casual,
    GrumpyVeteran,
    LoneWolf,
    Mentor,
    YoungApprentice,
    Socialite,
    Distracted,
    TrollMild,        // new 2026-08-21: rare, sarcastic/backhanded, light profanity
    TrollAggressive,   // new 2026-08-21: very rare, openly hostile about gameplay, vulgar
};

constexpr size_t kHsArchetypeCount = 15;

struct HsArchetypeInfo
{
    const char* enumName;       // e.g. "RAIDER_SERIOUS" — the prompt line's label, and hside_archetype's key
    std::string talksAbout;     // PLAN.md's "Talks about" column, verbatim (owned string -- loaded from DB, not a literal)
    float       care;           // §4.11 style-pass baseline, before combat offset/GUID jitter
    float       replyChance;    // 0..1, starting value — no consumer yet (§4.15's arbiter doesn't read archetype); stored, not wired
    uint32_t    verbosityCap;   // tokens, starting value — feeds Hs_CallLLM's maxTokens (capped by HearthsideChat.LLM.MaxTokens, never above it)
    uint32_t    spawnWeight;    // out of 100 across all fifteen entries; used by Hs_ArchetypeForBot's weighted draw
    bool        hasAbbrevOverride; // only TRADER sets this ("abbrev override: heavy" — PLAN.md line 1127)
    float       abbrevOverrideChance; // meaningful only if hasAbbrevOverride
    uint8_t     minLevel;       // §4.13 "Archetype eligibility, re-evaluated" — 0 = no lower bound
    uint8_t     maxLevel;       // 255 = no upper bound; only YOUNG_APPRENTICE sets this (low band only, 1-29)
    uint8_t     profanityLevel; // 0 = none, 1 = light (damn/hell/crap-tier), 2 = vulgar. New 2026-08-21 for TROLL_MILD/TROLL_AGGRESSIVE.
                                 // Orthogonal to `care` (a careful typer can still swear precisely) -- its own axis,
                                 // same "each axis draws its own jitter" reasoning §4.11 gives abbreviation/typo/caps.
};

// Replaces the whole in-memory archetype table. Called once at startup by
// hs_archetype_store.cpp's Hs_LoadArchetypesFromDb(); exposed here (rather
// than kept file-local) so a standalone test can seed fixture data the same
// way, without any AzerothCore dependency entering this file.
void Hs_SetArchetypeTable(const std::array<HsArchetypeInfo, kHsArchetypeCount>& table);

const HsArchetypeInfo& Hs_ArchetypeInfoFor(HsArchetype a);

// Deterministic weighted draw from the bot's GUID, restricted to the subset
// of the fifteen stock archetypes whose level requirement `level` satisfies
// (§4.13 "Archetype eligibility, re-evaluated" — a level-22 bot can never
// draw RAIDER_SERIOUS). Weights are renormalized over just the eligible
// subset each call, so lowering/raising a bot's level band changes what it
// can draw without needing any weight retuning. At least six of the fifteen
// entries carry no level requirement at all, so the eligible pool is never
// empty for any level 1-80.
//
// This function is pure — called fresh per request (hs_queue.cpp's
// WorkerLoop) from the bot's *current* level, so "redraw on level change"
// is automatic and costs nothing extra.
HsArchetype Hs_ArchetypeForBot(uint64_t botGuid, uint8_t level);

// The archetype delta line for the LLM prompt: "You mostly talk about:
// <talksAbout>." plus a profanity directive when profanityLevel > 0.
// Byte-identical for every bot sharing the archetype (until the table is
// reloaded), so it belongs after the shared baseline prefix (system rules +
// few-shot) and before per-bot-pair history (§4.11 "baseline persona plus
// archetype delta" — the delta is the only unshared part of the persona).
// The profanity directive is deliberately scoped to the game (gear,
// rotations, loot, other players' choices), never the real person on the
// other end of the conversation -- that boundary is part of the generated
// prompt text itself, not left to model judgement.
std::string Hs_ArchetypePromptLine(HsArchetype a);

#endif // MOD_HS_ARCHETYPE_H
