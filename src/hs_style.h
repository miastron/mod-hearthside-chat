#ifndef MOD_HS_STYLE_H
#define MOD_HS_STYLE_H

#include <cstdint>
#include <string>

// PLAN.md §4.11 style post-processor (build order step 6). A deterministic,
// zero-GPU transform applied to a model reply before it reaches history or
// delivery: strips LLM tells, then reshapes caps/terminal-punctuation/
// abbreviation and injects typos according to a per-bot `care` scalar
// (0.0 sloppy - 1.0 careful).
//
// Scope note: combat-based `care` modulation and the self-correction
// follow-up (both named in §4.11) are built. Two related items named in the
// same paragraphs are not, for lack of a hook to read them from: the
// trade/recruitment positive `care` offset and party-chat-during-an-
// encounter (this module only hooks /say and whisper — hs_config.h). See
// PROGRESS.md.

// Result of one style pass. `correction` is empty unless InjectTypos
// actually altered a word, in which case it holds that word's pre-typo
// (already cased/abbreviated) form — the exact text §4.11's self-correction
// follow-up sends as `*<correction>`. The ~5% roll and delivery delay for
// that follow-up are the queue's job (hs_queue.cpp), not this function's:
// this always reports what happened, unconditionally.
struct HsStyleResult
{
    std::string text;
    std::string correction;
};

// `baselineCare` comes from the bot's archetype (hs_archetype.h,
// Hs_ArchetypeInfoFor(...).care) as of step 7 — no more flat 0.5 placeholder.
// The +/-0.20 GUID jitter still applies on top, then `inCombat`'s fixed
// negative offset (§4.11 "context modulates care downward").
float Hs_StyleCareForBot(uint64_t botGuid, float baselineCare, bool inCombat);

// Per-call context that isn't per-word: the archetype's baseline `care`, an
// abbreviation-chance override (§4.11's "abbreviation is not always
// carelessness" — TRADER writes heavy abbreviation regardless of what its
// `care` band would otherwise pick; -1.0f means "no override, use the care
// band"), and whether the bot is in combat.
struct HsStyleContext
{
    float baselineCare;
    float abbrevOverrideChance;
    bool  inCombat;

    // §4.12's card fact sheet, "verbal_tic ... becomes a protected token in
    // §4.11's style pass" -- empty for an uncarded (or dormant-carded) bot,
    // in which case this is a no-op. Unlike hs_config.h's single-word
    // ProtectedWords() list, a tic may be a short phrase ("no worries"), so
    // it's masked as one literal span before word-splitting rather than
    // matched per-word.
    std::string verbalTic;
};

// Applies the full pass. botName/senderName protect those exact tokens from
// typo injection (§4.11 "never corrupt a token the player might act on");
// pass empty strings if unknown. Seeded from hash(botGuid, text), so a given
// (bot, message) pair transforms reproducibly (§4.11 "seed per message, not
// per bot").
HsStyleResult Hs_ApplyStyle(uint64_t botGuid, const std::string& botName,
                             const std::string& senderName, const std::string& text,
                             const HsStyleContext& ctx);

#endif // MOD_HS_STYLE_H
