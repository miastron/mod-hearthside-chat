#ifndef MOD_HS_STYLE_H
#define MOD_HS_STYLE_H

#include <cstdint>
#include <string>

// Style post-processor: a deterministic, zero-GPU transform applied to a
// model reply before it reaches history or delivery. Strips LLM tells, then
// reshapes caps/terminal-punctuation/abbreviation and injects typos
// according to a per-bot `care` scalar (0.0 sloppy - 1.0 careful).
//
// One thing `care` deliberately does not touch: an emphatic ALL-CAPS run
// ("GET OVER IT", "YEARS") is masked as a protected span and restored
// verbatim. Shouting is prosody the fine-tune decides per archetype, not a
// shift-key-hygiene artifact. See MaskCapsRuns in hs_style.cpp.
//
// Scope note: combat-based `care` modulation, the self-correction follow-up,
// and (§4.17) a positive `care` offset for Trade-channel WTS/WTB sightings
// are built, and apply on every hooked surface (/say, whisper, party/raid,
// guild, channels). What remains unbuilt is a *group-wide* encounter signal:
// `inCombat` below is the speaking bot's own IsInCombat(), so a bot standing
// out of combat while its group is mid-pull reads as calm. See
// Hs_StyleCareForBot in hs_style.cpp.

// Result of one style pass. `correction` is empty unless InjectTypos
// actually altered a word, in which case it holds that word's pre-typo
// (already cased/abbreviated) form: the exact text the self-correction
// follow-up sends as `*<correction>`. The roll and delivery delay for that
// follow-up are the queue's job (hs_queue.cpp); this always reports what
// happened.
struct HsStyleResult
{
    std::string text;
    std::string correction;
};

// `baselineCare` comes from the bot's archetype (hs_archetype.h,
// Hs_ArchetypeInfoFor(...).care). A +/-0.20 GUID jitter applies on top, then
// `inCombat`'s fixed negative offset, then `tradeCareOffset`, a small
// time-decayed positive magnitude (0 if the bot hasn't recently witnessed a
// WTS/WTB Trade-channel message, hs_channel.h's Hs_IsWtsWtb) computed at the
// call site, not here. This function stays pure addition.
float Hs_StyleCareForBot(uint64_t botGuid, float baselineCare, bool inCombat, float tradeCareOffset = 0.0f);

// §4.17: called from hs_handler.cpp's Trade-channel hook whenever a real
// player's message matches Hs_IsWtsWtb (hs_channel.h), for every bot
// currently a member of that channel instance, independent of whether any
// of them end up replying. Hs_TradeCareOffsetFor reads it back as a linearly
// decaying magnitude (0 once kTradeSightingWindowSeconds has elapsed, or if
// the bot has no recorded sighting), called at each HsStyleContext
// construction site the same way ctx.inCombat already is.
void  Hs_NoteTradeSighting(uint64_t botGuid);
float Hs_TradeCareOffsetFor(uint64_t botGuid);

// Per-call context that isn't per-word: the archetype's baseline `care`, an
// abbreviation-chance override (some archetypes, e.g. TRADER, write heavy
// abbreviation regardless of what their `care` band would otherwise pick;
// -1.0f means "no override, use the care band"), whether the bot is in
// combat, and the §4.17 Trade WTS/WTB `care` offset magnitude (0.0f if none).
struct HsStyleContext
{
    float baselineCare;
    float abbrevOverrideChance;
    bool  inCombat;
    float tradeCareOffset = 0.0f;

    // A carded bot's verbal tic, protected from the style pass. Empty for
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
