#ifndef MOD_HS_GROUNDED_H
#define MOD_HS_GROUNDED_H

#include <cstdint>
#include <string>
#include <vector>

// A lookup-and-template path sitting between the reflex check and the tier
// ceiling -- questions the realm can already answer truthfully from live
// Player*/DB state, with no GPU work and no chance of invention.
//
// Split the same way hs_reflex.h/hs_archetype.h are: this file is pure
// trigger-matching and template assembly, no AzerothCore dependency,
// standalone-testable. The actual Player*/DBC/GuildMgr lookups that resolve
// a matched kind to a fact string live in hs_handler.cpp (already
// AzerothCore-dependent), which calls back into Hs_BuildGroundedReply once
// it has the fact in hand.
//
// Phrases and templates load from SQL (hside_grounded_question/
// hside_grounded_template) rather than a compiled table, so new phrasing
// and reply variants can be authored without a rebuild -- same split as
// hs_archetype.h/hs_archetype_store.h: Hs_Set*Table replaces the whole
// in-memory table, called once at startup (and again on `.reload config`)
// by hs_grounded_store.cpp, and exposed here so a standalone test can seed
// fixture data the same way, without any AzerothCore dependency entering
// this file. The enum itself stays fixed in code -- hs_handler.cpp's
// per-kind Player*/DB resolution switches on it -- but every phrase and
// every reply variant is data.

enum class HsGroundedKind
{
    None,
    Mount,
    Level,
    Gold,
    Zone,
    Guild,
    Profession,
    Gear,

    // card_facts. Unlike the six above, these three only resolve for a
    // carded (ring-3) bot; the caller (hs_handler.cpp) treats "no active
    // card" or "field empty" the same way it treats Mount's "not observably
    // mounted" -- fall through rather than force a non-invented lacks-line
    // for a bot with no fact sheet at all.
    CurrentGoal,
    PlayedSince,
    Alt,

    // hside_memory, looked up and templated the same way, never carried in
    // a prompt. hasFact for all three comes from hs_memory_store.h's
    // pair-scoped queries, not live Player* state, so hs_handler.cpp's
    // resolution for these three is a DB lookup rather than a game-object
    // read, same shape as the three card-facts kinds above.
    RecallMet,      // "do you remember me"
    RecallDungeon,  // "what did we run"
    RecallGrouped,  // "have we grouped before"
};

// One row of hside_grounded_question: `phrase` is pre-normalized at
// authoring time the same way a trigger is normalized at match time
// (lowercase, single-spaced, no trailing punctuation) so the exact-match
// pass is a plain string comparison.
struct HsGroundedQuestionRow
{
    HsGroundedKind kind;
    std::string    phrase;
};

// One row of hside_grounded_template. `hasFact` selects which fact-state
// (has vs lacks) this row answers -- Mount/Level/Gold/Zone only ever get
// called with hasFact=true (see Hs_BuildGroundedReply below), so they only
// need has_fact=1 rows.
//
// `usesFact` distinguishes the two reply shapes this module needs:
//   true  -- reply is `prefix + fact + suffix` (the fact is interpolated)
//   false -- reply is `prefix` verbatim, a canned response; `fact` and
//            `suffix` are unused. Covers the flat has/lacks response pools
//            (Guild/Profession/Gear's lacks set, RecallMet/RecallGrouped's
//            has and lacks sets, RecallDungeon's lacks set).
struct HsGroundedTemplateRow
{
    HsGroundedKind kind;
    bool           hasFact;
    bool           usesFact;
    std::string    prefix;
    std::string    suffix;
};

// Replaces the whole in-memory question/template table. Called once at
// startup (and again on `.reload config`) by hs_grounded_store.cpp's
// Hs_LoadGroundedQuestionsFromDb()/Hs_LoadGroundedTemplatesFromDb().
// Exposed here (rather than kept file-local) so a standalone test can seed
// fixture data the same way, without any AzerothCore dependency entering
// this file. An empty table (DB not yet loaded, or hside_grounded_* came
// back empty) makes every lookup miss -- Hs_MatchGroundedQuestion returns
// None and Hs_BuildGroundedReply returns "", the same "fall through"
// degradation as a genuinely unmatched trigger.
void Hs_SetGroundedQuestionTable(const std::vector<HsGroundedQuestionRow>& rows);
void Hs_SetGroundedTemplateTable(const std::vector<HsGroundedTemplateRow>& rows);

// Matches `trigger` (the player's message, exactly as typed) against every
// loaded phrase. Whole-message, not substring -- same
// false-positive-is-worse-than-a-miss reasoning as hs_reflex.h.
//
// Two passes: an exact match first; if none, a typo-tolerance pass (bounded
// Levenshtein distance, capped at `fuzzyMaxDistance`) catches a slipped
// keystroke ("waht level are you") without reaching for a differently-
// worded or longer sentence -- a phrase whose length differs from the
// trigger by more than `fuzzyMaxDistance` is skipped outright. A candidate
// phrase from a different kind tied at the same distance makes the whole
// match ambiguous -> None, rather than guessing. `fuzzyMaxDistance == 0`
// disables the fallback pass (exact match only).
//
// `fuzzyMaxDistance` is caller-supplied (hs_handler.cpp's
// g_HsGroundedFuzzyMaxDistance, HearthsideChat.GroundedAnswers.
// FuzzyMaxDistance) rather than read here, same reasoning as
// HsStyleContext's caller-filled fields: this file stays free of
// hs_config.h, which pulls in AzerothCore's ScriptMgr.h.
HsGroundedKind Hs_MatchGroundedQuestion(const std::string& trigger, uint32_t fuzzyMaxDistance);

// Wraps `fact` (the caller's already-resolved lookup result, e.g. "42",
// "Elwynn Forest", "mining (300)") in one of the loaded template/response
// variants for `kind`+`hasFact`, chosen by hash(botGuid, trigger) -- seeded
// per message rather than per bot, same idiom as hs_reflex.h's Plain
// family. Returns "" if no matching row is loaded (unmatched kind, or the
// (kind, hasFact) pair has no rows -- e.g. CurrentGoal/PlayedSince/Alt have
// no lacks rows by design, so hasFact=false returns "" for them, same as
// an unmatched kind, letting the caller's existing "empty reply -> fall
// through" handling cover "no active card" for free).
std::string Hs_BuildGroundedReply(HsGroundedKind kind, bool hasFact, const std::string& fact,
                                    uint64_t botGuid, const std::string& trigger);

#endif // MOD_HS_GROUNDED_H
