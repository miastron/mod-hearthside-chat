#ifndef MOD_HS_IDENTITY_H
#define MOD_HS_IDENTITY_H

#include "hs_gen_validate.h" // reuses HsGenVerdict's shape: the "accepted/reason" contract
#include "hs_json.h"

#include <cstdint>
#include <string>

// Identity rings. Pure logic only, no AzerothCore dependency, same
// standalone-testable shape as hs_archetype.cpp/hs_gen_validate.cpp: score
// weights, the promotion threshold, and the card's fact-sheet validation
// gate. The DB-touching half (hside_identity reads/writes, the exclude-vector
// push into mod-playerbots) lives in hs_identity_store.h, which is
// AzerothCore-dependent and calls into this file rather than duplicating its
// logic.

// Weight table for the four chat-surface signals plus one event-based signal
// reachable through the existing dungeon-completion opener hook. Ordered by
// how deliberately a bot was addressed: a whisper is 1:1 and unmissable, a
// party/raid line is small-group, guild is larger and more ambient, /say is
// the most incidental (anyone in range hears it, not just this bot).
constexpr uint32_t kHsScoreWeightWhisper         = 4;
constexpr uint32_t kHsScoreWeightPartyRaid       = 3;
constexpr uint32_t kHsScoreWeightGuild           = 2;
constexpr uint32_t kHsScoreWeightSay             = 1;
constexpr uint32_t kHsScoreWeightDungeonComplete = 2;

// A settled design constant, not an operator knob: over-promotion has no
// expensive failure mode, so the bar is kept low.
constexpr uint32_t kHsPromotionThreshold = 8;

// Decay/dormancy windows, shipped as compiled constants rather than config:
// being wrong costs one flag flip on the next sweep, not a structural
// failure. A week of silence before score starts drifting down, so one quiet
// weekend doesn't undo real conversation; a season of silence before a card
// goes dormant.
constexpr uint32_t kHsScoreDecayGraceDays    = 7;
constexpr uint32_t kHsScoreDecayPointsPerDay = 1;  // applied once/day past the grace window, floored at 0
constexpr uint32_t kHsCardDormancyDays       = 30;

// The card's fact sheet, eight fields. main_focus/played_since/
// preferred_content/guild_stance are enum-constrained; current_goal/
// held_opinion are freeform but format-checked only, not validated against
// acore_world (the same place-name-risk tradeoff scripted dialogue accepts
// elsewhere). alt is checked against the real 10-class list; verbal_tic is a
// short literal, empty meaning "none".
namespace HsCardFacts
{
    // main_focus has no fixed external value list like played_since/
    // preferred_content; this set is authored to be small, unambiguous,
    // and level-band-gateable.
    extern const char* const kMainFocusValues[];
    extern const size_t      kMainFocusCount;

    extern const char* const kPlayedSinceValues[]; // vanilla | bc | wrath
    extern const size_t      kPlayedSinceCount;

    extern const char* const kPreferredContentValues[]; // 5-mans | raids | pvp | solo
    extern const size_t      kPreferredContentCount;

    // guild_stance is a two-value enum, not freeform prose, so "must agree
    // with the bot's actual guild row" is mechanically checkable instead of
    // requiring sentiment parsing.
    extern const char* const kGuildStanceValues[]; // guilded | unguilded
    extern const size_t      kGuildStanceCount;

    // The class vocabulary moved to hs_class.h (HsClass::kNames), shared
    // with hs_corpus.cpp's Hs_ClassNameFor rather than re-spelled here
    // (review item 19).
}

// True if `value` is main_focus-plausible for `level`: raiding/dailies/
// achievements require the high/endgame band, leveling doesn't fit endgame.
// Mirrors hs_corpus.h's Hs_LevelBandFor bands (low/mid/high/endgame) without
// depending on that header, to keep this file's own dependency surface at
// zero.
bool Hs_MainFocusPlausibleForLevel(const std::string& value, uint8_t level);

// Runs the whole gate: presence/type of all eight keys, enum membership,
// main_focus band plausibility, guild_stance agreement with `hasGuild`, alt
// against the real class list, and format checks (non-empty, length, no
// markdown/quote chars) on current_goal/held_opinion/verbal_tic. Stops at
// the first failure, same "cost order, first failure wins" shape as
// Hs_EvaluateCandidate.
// ownClassName is the bot's own class in Hs_ClassNameFor's lowercase
// vocabulary ("death knight", not "Death Knight"). Empty means the caller
// could not resolve it, and the alt-vs-own-class check (review C10) is
// skipped rather than guessed at.
HsGenVerdict Hs_ValidateCardFacts(const hs_json& facts, uint8_t level, bool hasGuild,
                                   const std::string& ownClassName = "");

// The two generation prompts. Pure string-building, deliberately minimal
// wrappers around the archetype's own "talks about" line and the level/
// guild context the caller already resolved, not invented biography. Same
// discipline as hs_archetype.h's Hs_ArchetypePromptLine.
std::string Hs_BuildVoiceBlockPrompt(const std::string& archetypeTalksAbout);

// One card fact, asked on its own -- 2026-09-14. Replaces
// Hs_BuildCardFactsPrompt, which asked for all eight keys at once as a single
// line of JSON.
//
// That prompt cannot work against the model this module targets. Measured on
// the live endpoint with the real trigger, the tuned Llama-3.2-1B returned
// valid JSON 0 times in 10, under the shipped prompt shape and under the
// pre-2026-09-14 one alike; it answers "main_focus: gathering and explaining
// gearing_up", because a chat-only fine-tune answers in chat register. The
// same model returns valid JSON for a bare, unframed JSON request, so the
// capability is suppressed by the WoW-chat framing rather than absent -- but
// the framing is not optional here, so the JSON ask is.
//
// Asking one short question at a time is what the tune *is* good at: every
// training row is a single short line answering a single short prompt. The
// caller assembles the eight answers into the same object Hs_ValidateCardFacts
// already checks, so the validator is unchanged.
//
// `prompt` empty means this field is not asked at all and `fixed` holds the
// value -- guild_stance is a fact the server already has from the guild row,
// and asking a model to restate it can only introduce a disagreement the
// validator then rejects the whole card for.
// `grammar` is a GBNF alternation of exactly the values that are valid for
// THIS bot, passed to Hs_CallLLM so the sampler cannot emit anything else.
// Empty for the freeform fields, which have no vocabulary to constrain.
//
// Built per bot on purpose, not from the static enum tables: main_focus omits
// the values Hs_MainFocusPlausibleForLevel rejects at this level, and alt
// omits the bot's own class. That turns two of the deterministic card
// rejections behind the generator livelock (review C10's
// alt_is_the_characters_own_class, C11's main_focus_not_plausible_for_level)
// from "caught by the validator, card thrown away, bot retried" into
// "unreachable".
struct HsCardFactAsk
{
    char const* key;
    std::string prompt;
    std::string grammar;
    std::string fixed;
};

constexpr uint32_t kHsCardFactCount = 8;

// `index` in [0, kHsCardFactCount).
HsCardFactAsk Hs_CardFactAsk(uint32_t index, const std::string& archetypeTalksAbout, uint8_t level,
                              bool hasGuild, const std::string& guildName,
                              const std::string& ownClassName = "");

// The shared user turn for every asked field.
char const* Hs_CardFactTrigger();

// Trims a raw one-line model answer into the value the validator expects:
// whitespace, wrapping quotes, a trailing period, leading "key:" echo, and
// for the enum fields a lowercase/underscore normalisation ("Gearing Up" ->
// "gearing_up"). Parsing a reply, not judging it -- a value this cannot
// rescue still fails Hs_ValidateCardFacts on its own merits.
std::string Hs_NormalizeCardFactValue(uint32_t index, std::string raw);

// verbal_tic becomes a protected token in the style pass (hs_style.cpp).
// Empty return means no tic: a missing key, wrong type, or empty string
// value all collapse to the same case.
std::string Hs_ExtractVerbalTic(const hs_json& facts);

// Looks up one field's string value, empty if absent/wrong type. Shared by
// hs_identity_store.cpp's card-facts grounded-answer lookup (current_goal,
// played_since, alt) so the JSON access pattern lives in one place.
std::string Hs_CardFactField(const hs_json& facts, const std::string& fieldName);

// The voice-block quality bound: short prose, no markdown/quote characters,
// non-empty. Deliberately not hs_gen_validate.h's Hs_QualityGate: that
// gate's 10-180 char bound and question-shape rejection are tuned for
// closed corpus lines, not a ~50-token persona paragraph. Conflating the
// two would tie card validation to unrelated corpus tuning.
HsGenVerdict Hs_ValidateVoiceBlock(const std::string& text);

#endif // MOD_HS_IDENTITY_H
