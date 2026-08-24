#ifndef MOD_HS_ARCHETYPE_H
#define MOD_HS_ARCHETYPE_H

#include <array>
#include <cstdint>
#include <string>

// Archetype content (weights, care, verbosity, etc.) loads from SQL
// (hside_archetype) rather than a compiled table, so it can be retuned
// without touching bots. The enum itself stays fixed in code -- ring-
// eligibility logic and Hs_ArchetypeInfoFor's index-by-enum-value depend on
// a stable ordinal -- but every tunable column is data.
//
// Split like hs_identity.h/hs_identity_store.h: this file is pure logic (the
// in-memory table plus the weighted draw), no AzerothCore dependency,
// standalone-testable. hs_archetype_store.h/.cpp owns the hside_archetype
// query and calls Hs_SetArchetypeTable once at startup.
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
    Mentor,
    YoungApprentice,
    Socialite,
    TrollMild,        // rare, sarcastic/backhanded, light profanity
    TrollAggressive,  // very rare, openly hostile about gameplay, vulgar
};

constexpr size_t kHsArchetypeCount = 13;

struct HsArchetypeInfo
{
    const char* enumName;       // e.g. "RAIDER_SERIOUS" -- the prompt line's label, and hside_archetype's key
    std::string talksAbout;     // verbatim archetype description text (owned string -- loaded from DB, not a literal)
    float       care;           // style-pass baseline, before combat offset/GUID jitter
    float       distractedChance; // 0..1; rolled per completed tier-2 reply by hs_queue.cpp's WorkerLoop.
                                 // On a hit the bot delivers a canned "sorry, was afk" line first, then the
                                 // real reply a full typing delay later -- a per-reply behavior every
                                 // personality shows occasionally, which is why the DISTRACTED archetype
                                 // was retired in favor of this column. Replaced the old `replyChance`
                                 // (2026-08-24): a reply-chance roll reads as being ignored on a realm
                                 // that is mostly bots, where a late reply beats silence.
    uint32_t    verbosityCap;   // tokens; feeds Hs_CallLLM's maxTokens, capped by HearthsideChat.LLM.MaxTokens (never raised above it)
    uint32_t    spawnWeight;    // out of 100 across all thirteen entries; used by Hs_ArchetypeForBot's weighted draw
    bool        hasAbbrevOverride; // only TRADER sets this
    float       abbrevOverrideChance; // meaningful only if hasAbbrevOverride
    uint8_t     minLevel;       // 0 = no lower bound
    uint8_t     maxLevel;       // 255 = no upper bound; only YOUNG_APPRENTICE sets this (low band only, 1-29)
    uint8_t     profanityLevel; // 0 = none, 1 = light (damn/hell/crap-tier), 2 = vulgar (TROLL_MILD/TROLL_AGGRESSIVE only)
                                 // Orthogonal to `care` -- a careful typer can still swear precisely; its own
                                 // independent axis, same as abbreviation/typo/caps.
    uint32_t    typingBaseMs;     // hs_queue.cpp's tier-2 typing-delay formula: flat "notice and start typing" cost
    uint32_t    typingPerCharMs;  // ms per character of the styled reply; a hasty archetype types faster, not just shorter
                                   // (HearthsideChat.TypingDelay.Enable/MaxMs is the kill switch and ceiling, same relationship as verbosityCap/LLM.MaxTokens)
};

// Replaces the whole in-memory archetype table. Called once at startup by
// hs_archetype_store.cpp's Hs_LoadArchetypesFromDb(); exposed here (rather
// than kept file-local) so a standalone test can seed fixture data the same
// way, without any AzerothCore dependency entering this file.
void Hs_SetArchetypeTable(const std::array<HsArchetypeInfo, kHsArchetypeCount>& table);

// Returns a *copy*, not a reference into the in-memory table: the table is
// replaced wholesale on every `.reload config` (Hs_SetArchetypeTable) while
// the queue-worker and generator threads hold their lookup across a
// multi-second LLM call, and HsArchetypeInfo::talksAbout is an owned string
// whose buffer that replace would free under them.
HsArchetypeInfo Hs_ArchetypeInfoFor(HsArchetype a);

// Reverse lookup by enum_name against whatever's currently loaded
// (Hs_SetArchetypeTable), so a caller like hs_command.cpp's `.hearthside
// archetype` GM command validates against live data instead of a second,
// separately-maintained name list. Returns false (out untouched) if no
// loaded entry's enumName matches.
bool Hs_ArchetypeForName(const std::string& enumName, HsArchetype& out);

// GM override: pins a bot to one specific stock archetype, bypassing the
// GUID-weighted draw entirely (Hs_ArchetypeForBot checks this first, no
// level-eligibility filter -- an explicit pin is trusted as-is). In-memory
// only and thread-safe; hs_archetype_store.cpp owns loading/persisting
// these across restarts, same split as the archetype table itself.
void Hs_SetArchetypeOverride(uint64_t botGuid, HsArchetype archetype);
void Hs_ClearArchetypeOverride(uint64_t botGuid);

// Deterministic weighted draw from the bot's GUID, restricted to the subset
// of the fifteen stock archetypes whose level requirement `level` satisfies
// -- a level-22 bot can never draw RAIDER_SERIOUS. Weights are renormalized
// over just the eligible subset each call, so changing a bot's level band
// changes what it can draw without any weight retuning. At least six of the
// fifteen entries carry no level requirement, so the eligible pool is never
// empty for any level 1-80. Returns the GM override (above) first if one is
// set for this bot, skipping the draw entirely.
//
// Pure and called fresh per request (hs_queue.cpp's WorkerLoop) from the
// bot's current level, so redrawing on a level change is automatic and
// costs nothing extra.
HsArchetype Hs_ArchetypeForBot(uint64_t botGuid, uint8_t level);

// The archetype delta line for the LLM prompt: "You mostly talk about:
// <talksAbout>." plus a profanity directive when profanityLevel > 0.
// Byte-identical for every bot sharing the archetype (until the table is
// reloaded), so it belongs after the shared baseline prefix (system rules +
// few-shot) and before per-bot-pair history -- it's the only unshared part
// of the persona. The profanity directive is deliberately scoped to the
// game (gear, rotations, loot, other players' choices), never the real
// person on the other end -- that boundary is part of the generated prompt
// text itself, not left to model judgement.
std::string Hs_ArchetypePromptLine(HsArchetype a);

#endif // MOD_HS_ARCHETYPE_H
