#ifndef MOD_HS_REFLEX_H
#define MOD_HS_REFLEX_H

#include <cstdint>
#include <string>

// Tier 0: one hardcoded pattern table, no AzerothCore dependency (pure
// string matching), covering three curated-reply families that never touch
// the LLM:
//
//   - the high-volume social reflex vocabulary: gz / ty / inv / sum / lol / wb
//   - the "are you a bot?" question, the module's most-scrutinised line,
//     so it gets an operator-configurable mode
//   - the personal-probe privacy deflection set: "where are you from",
//     "what do you do", "how old are you", "m or f"
//
// All three share this one table and one arbitration path. Arbitration
// itself is not this file's job: hs_arbiter.h already runs before any bot
// reaches the caller, so every bot passed in here has already been selected
// to reply. This file only decides whether the *trigger* text is a reflex
// pattern and, if so, what the canned reply is.
//
// Deliberately out of scope: no identity-store access, no history write, no
// score. Tier 0 is pure input to output.

// Operator-facing mode for the "are you a bot?" reflex. Default is `Wink`:
// a non-answer is not a lie, where "no, I'm a real player" is one.
enum class HsBotQuestionMode
{
    Wink,     // maybe! / shh... don't tell anyone / huh? / what?
    Deflect,  // huh? / what? only, the more evasive subset of Wink
    Silent,   // matched, but the utterance is dropped rather than answered
    Admit,    // operator wants the realm to be straightforward about it
};

// Unrecognised values fall back to Wink, the documented default.
HsBotQuestionMode Hs_ParseBotQuestionMode(const std::string& value);

enum class HsReflexKind
{
    None,          // no match, caller falls through to whatever tier is next
    Plain,         // gz / ty / inv / sum / lol / wb
    BotQuestion,   // "are you a bot?" family
    PersonalProbe, // "where are you from" family
};

struct HsReflexMatch
{
    HsReflexKind kind = HsReflexKind::None;
    // Chosen reply text, already selected from the curated set. Empty with
    // kind != None means "matched, but this roll came up silent" (Silent
    // mode, or the PersonalProbe pool's no-reply member). The caller must
    // treat that as handled, not as a miss to fall through on.
    std::string text;
};

// Matches `trigger` (the player's message, exactly as typed) against the
// tier-0 pattern table. Matching is whole-message, not substring: a long
// sentence that happens to contain "bot" or "ty" never matches, since a
// false positive is worse than a miss for every entry here.
//
// botGuid/senderGuid seed the BotQuestion/PersonalProbe families so a given
// bot gives a given player the same stock answer every time they ask
// (re-asking and getting the same wry answer reads as more human than a
// fresh one). The Plain family seeds on hash(botGuid, trigger) instead:
// gz/ty/etc. are far too frequent for a fixed per-player answer to go
// unnoticed, so it seeds per message instead of per bot.
HsReflexMatch Hs_MatchReflex(const std::string& trigger, uint64_t botGuid, uint64_t senderGuid,
                              HsBotQuestionMode botQuestionMode);

#endif // MOD_HS_REFLEX_H
