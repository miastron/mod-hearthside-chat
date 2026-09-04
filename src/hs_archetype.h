#ifndef MOD_HS_ARCHETYPE_H
#define MOD_HS_ARCHETYPE_H

#include <array>
#include <cstdint>
#include <string>

// Archetype content (weights, care, verbosity, etc.) loads from SQL
// (hside_archetype) rather than a compiled table, so it can be retuned
// without touching bots. The enum itself stays fixed in code (ring-
// eligibility logic and Hs_ArchetypeInfoFor's index-by-enum-value depend on
// a stable ordinal), but every tunable column is data.
//
// Split like hs_identity.h/hs_identity_store.h: this file is pure logic (the
// in-memory table plus the weighted draw), no AzerothCore dependency,
// standalone-testable. hs_archetype_store.h/.cpp owns the hside_archetype
// query and calls Hs_SetArchetypeTable once at startup.
//
// Declaration order here must match kEnumNames' order in hs_archetype.cpp:
// Hs_ArchetypeInfoFor indexes the in-memory table directly by this enum's
// underlying value rather than switching on it.
enum class HsArchetype
{
    RaiderSerious,
    RaiderCasual,
    PvpSerious,
    PvpCasual,
    Trader,
    Casual,
    GrumpyVeteran,
    Mentor,
    YoungApprentice,
    Socialite,
    TrollMild,        // rare, sarcastic/backhanded, light profanity
    TrollAggressive,  // very rare, openly hostile about gameplay, vulgar
};

constexpr size_t kHsArchetypeCount = 12;

struct HsArchetypeInfo
{
    const char* enumName;       // e.g. "RAIDER_SERIOUS", the prompt line's label, and hside_archetype's key
    std::string talksAbout;     // verbatim archetype description text (owned string, loaded from DB, not a literal)
    float       care;           // style-pass baseline, before combat offset/GUID jitter
    float       distractedChance; // 0..1; rolled per completed tier-2 reply by hs_queue.cpp's WorkerLoop.
                                 // On a hit the bot delivers a canned "sorry, was afk" line first, then the
                                 // real reply a full typing delay later, a per-reply behavior every
                                 // personality shows occasionally, which is why the DISTRACTED archetype
                                 // was retired in favor of this column. Replaced the old `replyChance`
                                 // (2026-08-24): a reply-chance roll reads as being ignored on a realm
                                 // that is mostly bots, where a late reply beats silence.
    uint32_t    verbosityCap;   // tokens; feeds Hs_CallLLM's maxTokens, capped by HearthsideChat.LLM.MaxTokens (never raised above it)
    uint32_t    spawnWeight;    // out of 100 across all twelve entries; used by Hs_ArchetypeForBot's weighted draw
    bool        hasAbbrevOverride; // only TRADER sets this
    float       abbrevOverrideChance; // meaningful only if hasAbbrevOverride
    // No minLevel/maxLevel: level no longer gates the draw (see
    // Hs_ArchetypeForBot). hside_archetype's min_level/max_level columns are
    // left in the schema -- editing a base/*.sql file that has already been
    // hash-tracked would make UpdateFetcher re-apply it -- but nothing reads
    // them any more.
    uint8_t     profanityLevel; // 0 = none, 1 = light (damn/hell/crap-tier), 2 = vulgar (TROLL_MILD/TROLL_AGGRESSIVE only)
                                 // Orthogonal to `care`: a careful typer can still swear precisely. Its own
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
// GUID-weighted draw entirely (Hs_ArchetypeForBot checks this first).
// In-memory only and thread-safe; hs_archetype_store.cpp owns loading/
// persisting these across restarts, same split as the archetype table
// itself.
void Hs_SetArchetypeOverride(uint64_t botGuid, HsArchetype archetype);
void Hs_ClearArchetypeOverride(uint64_t botGuid);

// Deterministic weighted draw from the bot's GUID over all twelve stock
// archetypes. Returns the GM override (above) first if one is set for this
// bot, skipping the draw entirely.
//
// A function of the GUID alone (review C1, 2026-09-03). It used to take the
// bot's level and renormalize the weighted draw over only the
// level-eligible subset, which meant both the modulus and the cumulative
// ordering changed at every band boundary, so *any* bot could land on a
// different archetype after a ding -- not just one holding a
// band-restricted archetype. That interacted badly with the identity
// system: a carded bot's card_voice is generated from the archetype's
// talksAbout at generation time and never regenerated, while Hs_RetireCard
// only fires on a level *drop*, so a bot dinging 59 -> 60 kept a voice
// block written for a personality it no longer had.
//
// The gating itself is gone rather than merely pinned, per operator
// direction: players routinely level alts after reaching max level, so a
// level-14 character talking about a level-80 raid is not out of the
// ordinary, and the level bands were buying realism the realm does not
// actually have. Archetype is now a stable property of the character for
// its whole life.
//
// Still pure and still called fresh per request (hs_queue.cpp's
// WorkerLoop), so it costs nothing extra to keep recomputing rather than
// caching.
HsArchetype Hs_ArchetypeForBot(uint64_t botGuid);

// The archetype delta line for the LLM prompt: "You mostly talk about:
// <talksAbout>." plus a profanity directive when profanityLevel > 0.
// Byte-identical for every bot sharing the archetype (until the table is
// reloaded), so it belongs after the shared baseline prefix (system rules +
// few-shot) and before per-bot-pair history: it's the only unshared part
// of the persona. The profanity directive is deliberately scoped to the
// game (gear, rotations, loot, other players' choices), never the real
// person on the other end. That boundary is part of the generated prompt
// text itself, not left to model judgement.
std::string Hs_ArchetypePromptLine(HsArchetype a);

#endif // MOD_HS_ARCHETYPE_H
