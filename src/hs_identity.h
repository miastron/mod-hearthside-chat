#ifndef MOD_HS_IDENTITY_H
#define MOD_HS_IDENTITY_H

#include "hs_gen_validate.h" // reuses HsGenVerdict's shape -- same "accepted/reason" contract
#include "hs_json.h"

#include <cstdint>
#include <string>

// PLAN.md §4.12 / §7 step 15 -- identity rings. Pure logic only, no
// AzerothCore dependency, same standalone-testable shape as hs_archetype.cpp/
// hs_gen_validate.cpp: score weights, the promotion threshold, and the card's
// fact-sheet validation gate. The DB-touching half (hside_identity reads/
// writes, the exclude-vector push into mod-playerbots) lives in
// hs_identity_store.h, which is AzerothCore-dependent and calls into this
// file rather than duplicating its logic.

// §4.12 "count conversation, not groups" weight table. Only the two signals
// this module actually has a hook for -- whisper and /say -- plus the one
// event-based signal reachable through step 13's existing dungeon-completion
// opener hook. Guild(2) and party/raid say(3) are named in the same table
// but have no chat-surface hook in this module yet (same gap class as
// hs_config.h's "party/raid/guild surfaces are not wired up"); adding them
// is a hook problem, not a scoring problem, so they're not approximated here.
constexpr uint32_t kHsScoreWeightWhisper         = 4;
constexpr uint32_t kHsScoreWeightSay             = 1;
constexpr uint32_t kHsScoreWeightDungeonComplete = 2;

// §4.12 "set the bar low -- around 6-10 weighted points." A settled design
// constant per that section's own framing ("over-promotion has no expensive
// failure mode"), not an operator knob -- same class as the self-correction
// chance and the opener cooldown/fire-chance constants.
constexpr uint32_t kHsPromotionThreshold = 8;

// §4.12 decay/dormancy (§7 step 17). §6 flags both windows as "needs a live
// realm to judge," same as the promotion threshold above -- and that one
// shipped as a compiled constant anyway, on the same reasoning: being wrong
// costs one flag flip on the next sweep, not a structural failure. A week of
// silence before score starts drifting down (so one quiet weekend doesn't
// undo real conversation); a season of silence before a card goes dormant
// (§4.12's own example is "reached 8 points last spring... not spoken to
// since" -- a season-scale window, not a week).
constexpr uint32_t kHsScoreDecayGraceDays    = 7;
constexpr uint32_t kHsScoreDecayPointsPerDay = 1;  // applied once/day past the grace window, floored at 0
constexpr uint32_t kHsCardDormancyDays       = 30;

// §4.12's fact sheet, eight fields. main_focus/played_since/preferred_content/
// guild_stance are enum-constrained; current_goal/held_opinion are freeform
// but format-checked only -- PLAN.md asks that their subject be checkable
// against acore_world, but building that lookup is the same rule-engine step
// 14 already declined for scripted dialogue's place-name risk (§4.16's own
// "the register is the fix... cannot be cheaply gated on one tag axis").
// Scoped down identically here rather than re-litigating it: format-checked,
// residual risk named, not solved. alt is checked against the real 10-class
// list; verbal_tic is a short literal, empty meaning "none".
namespace HsCardFacts
{
    // main_focus has no PLAN-given value list (unlike played_since/
    // preferred_content, which are spelled out exactly) -- this set is
    // authored to be small, unambiguous, and level-band-gateable, not lifted
    // from the plan verbatim, since the plan only specifies "enum, and must
    // be plausible for the level band."
    extern const char* const kMainFocusValues[];
    extern const size_t      kMainFocusCount;

    extern const char* const kPlayedSinceValues[]; // vanilla | bc | wrath
    extern const size_t      kPlayedSinceCount;

    extern const char* const kPreferredContentValues[]; // 5-mans | raids | pvp | solo
    extern const size_t      kPreferredContentCount;

    // guild_stance is made a two-value enum (rather than freeform prose)
    // specifically so "must agree with the bot's actual guild row" is
    // mechanically checkable rather than requiring sentiment parsing.
    extern const char* const kGuildStanceValues[]; // guilded | unguilded
    extern const size_t      kGuildStanceCount;

    extern const char* const kClassNames[]; // the 10 playable WotLK classes, lowercase
    extern const size_t      kClassNameCount;
}

// True if `value` is main_focus-plausible for `level` -- e.g. raiding/
// dailies/achievements require the high/endgame band, leveling doesn't fit
// endgame. Mirrors hs_corpus.h's Hs_LevelBandFor bands (low/mid/high/
// endgame) without depending on that header, to keep this file's own
// dependency surface at zero.
bool Hs_MainFocusPlausibleForLevel(const std::string& value, uint8_t level);

// Runs the whole gate: presence/type of all eight keys, enum membership,
// main_focus band plausibility, guild_stance agreement with `hasGuild`, alt
// against the real class list, and format checks (non-empty, length, no
// markdown/quote chars) on current_goal/held_opinion/verbal_tic. Stops at
// the first failure, same "cost order, first failure wins" shape as
// Hs_EvaluateCandidate.
HsGenVerdict Hs_ValidateCardFacts(const hs_json& facts, uint8_t level, bool hasGuild);

// The two generation prompts. Pure string-building, deliberately minimal
// wrappers around the archetype's own "talks about" line and the level/
// guild context the caller already resolved -- not invented biography, same
// discipline as hs_archetype.h's Hs_ArchetypePromptLine.
std::string Hs_BuildVoiceBlockPrompt(const std::string& archetypeTalksAbout);
std::string Hs_BuildCardFactsPrompt(const std::string& archetypeTalksAbout, uint8_t level, bool hasGuild,
                                     const std::string& guildName);

// §4.12: "verbal_tic ... becomes a protected token in §4.11's style pass."
// Empty return means no tic (facts missing the key, wrong type, or an empty
// string value all collapse to the same "nothing to protect" case).
std::string Hs_ExtractVerbalTic(const hs_json& facts);

// Looks up one field's string value, empty if absent/wrong type. Shared by
// hs_identity_store.cpp's card-facts grounded-answer lookup (current_goal,
// played_since, alt) so the JSON access pattern lives in one place.
std::string Hs_CardFactField(const hs_json& facts, const std::string& fieldName);

// The voice-block quality bound: short prose, no markdown/quote characters,
// non-empty. Deliberately not hs_gen_validate.h's Hs_QualityGate -- that
// gate's 10-180 char bound and question-shape rejection are tuned for
// closed corpus lines, not a ~50-token persona paragraph, and conflating the
// two would tie card validation to unrelated corpus tuning.
HsGenVerdict Hs_ValidateVoiceBlock(const std::string& text);

#endif // MOD_HS_IDENTITY_H
