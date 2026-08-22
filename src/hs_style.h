#ifndef MOD_HS_STYLE_H
#define MOD_HS_STYLE_H

#include <cstdint>
#include <string>

// Style post-processor: a deterministic, zero-GPU transform applied to a
// model reply before it reaches history or delivery. Strips LLM tells, then
// reshapes caps/terminal-punctuation/abbreviation and injects typos
// according to a per-bot `care` scalar (0.0 sloppy - 1.0 careful).
//
// Scope note: combat-based `care` modulation and the self-correction
// follow-up are built. Two related cases are not, for lack of a hook to read
// them from: a positive `care` offset for trade/recruitment posts, and
// party chat during an encounter -- this module only hooks /say and whisper
// (hs_config.h).

// Result of one style pass. `correction` is empty unless InjectTypos
// actually altered a word, in which case it holds that word's pre-typo
// (already cased/abbreviated) form -- the exact text the self-correction
// follow-up sends as `*<correction>`. The roll and delivery delay for that
// follow-up are the queue's job (hs_queue.cpp); this always reports what
// happened, unconditionally.
struct HsStyleResult
{
    std::string text;
    std::string correction;
};

// `baselineCare` comes from the bot's archetype (hs_archetype.h,
// Hs_ArchetypeInfoFor(...).care). A +/-0.20 GUID jitter applies on top, then
// `inCombat`'s fixed negative offset.
float Hs_StyleCareForBot(uint64_t botGuid, float baselineCare, bool inCombat);

// Per-call context that isn't per-word: the archetype's baseline `care`, an
// abbreviation-chance override (some archetypes, e.g. TRADER, write heavy
// abbreviation regardless of what their `care` band would otherwise pick;
// -1.0f means "no override, use the care band"), and whether the bot is in
// combat.
struct HsStyleContext
{
    float baselineCare;
    float abbrevOverrideChance;
    bool  inCombat;

    // A carded bot's verbal tic, protected from the style pass -- empty for
    // an uncarded (or dormant-carded) bot, in which case this is a no-op.
    // Unlike hs_config.h's single-word ProtectedWords() list, a tic may be a
    // short phrase ("no worries"), so it's masked as one literal span before
    // word-splitting rather than matched per-word.
    std::string verbalTic;
};

// Applies the full pass. botName/senderName protect those exact tokens from
// typo injection; pass empty strings if unknown. Seeded from hash(botGuid,
// text), so a given (bot, message) pair transforms reproducibly.
HsStyleResult Hs_ApplyStyle(uint64_t botGuid, const std::string& botName,
                             const std::string& senderName, const std::string& text,
                             const HsStyleContext& ctx);

#endif // MOD_HS_STYLE_H
