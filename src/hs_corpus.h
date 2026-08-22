#ifndef MOD_HS_CORPUS_H
#define MOD_HS_CORPUS_H

#include <cstdint>
#include <string>

// When a surface's MaxTier ceiling permits corpus but not inference, this
// answers instead of falling straight to silence. Weighted anti-repeat pick
// from a category the bot's class/level qualifies for, synchronous like
// TryReflex/TryGrounded in hs_handler.cpp -- zero GPU work, so no queue, no
// bucket, no cooldown; only the ceiling->inference branch touches the token
// bucket.
//
// Scoped to the tag axes the seeded content actually uses (none, class,
// level_band) and to channel IS NULL categories -- the /say and
// direct-reply set (channel_* categories are ambient content for a
// channel-chat hook that doesn't exist yet, not this fallback).
// faction/zone-axis categories are skipped until a category actually uses
// one; no plumbing for a signal nothing reads yet.
//
// Returns empty if no eligible category has a matching row (schema not
// installed, or nothing fits this bot's class/level) -- caller falls
// through to silence like any other "nothing to say" path.
//
// `hasActiveCard`: a card_gated=1 category is only eligible when true --
// its rows use %main_focus/%current_goal, which only resolve for a carded
// bot. False (the overwhelming majority of bots) simply removes those
// categories from the eligible set, same as the class/level_band tag
// filtering.
std::string Hs_SelectCorpusLine(uint8_t botClass, uint8_t botLevel, bool hasActiveCard);

// An opener fires off a specific shared-context trigger (hs_opener.cpp)
// that already knows which category applies -- "group formed" wants
// opener_group_formed, not a random pick among all eligible categories.
// Same anti-repeat pick and exposure bookkeeping as Hs_SelectCorpusLine,
// scoped to one named category. Returns empty if the category doesn't
// exist, isn't flagged is_opener, uses an unsupported tag axis (faction/
// zone), or has no row matching this bot's class/level.
std::string Hs_SelectOpenerLine(const std::string& categoryName, uint8_t botClass, uint8_t botLevel);

// The four level_band_tag labels used by chat_levelband_musing's seeded
// rows: low 1-19, mid 20-59, high 60-79, endgame 80 (the level cap) --
// lined up with WotLK's own pacing (Outland opens at 58, Northrend at 68,
// raiding/dailies only exist at the level-80 cap) rather than an even
// split.
//
// Inline and header-only (no hs_corpus.cpp dependency, which pulls in
// AzerothCore's DatabaseEnv.h) so a standalone test harness can include
// just this header, same pattern as hs_tier.h's HsParseTier/HsTierAllows.
inline std::string Hs_LevelBandFor(uint8_t level)
{
    if (level >= 80) return "endgame";
    if (level >= 60) return "high";
    if (level >= 20) return "mid";
    return "low";
}

// Card-only placeholder resolution: literal substring replacement, not a
// template engine -- there are exactly two card-only placeholders
// (hs_gen_validate.cpp's kCardOnlyPlaceholders), and a card-gated category
// is only ever selected for a bot that actually has both. `mainFocus`/
// `currentGoal` empty is a no-op for that token (defensive; shouldn't
// happen given the card_gated gating above). Pure and header-only, no
// AzerothCore dependency, same reasoning as Hs_LevelBandFor.
inline std::string Hs_ResolveCardPlaceholders(std::string text, const std::string& mainFocus,
                                               const std::string& currentGoal)
{
    auto replaceAll = [](std::string& s, const std::string& token, const std::string& value)
    {
        if (value.empty())
            return;
        size_t pos = 0;
        while ((pos = s.find(token, pos)) != std::string::npos)
        {
            s.replace(pos, token.size(), value);
            pos += value.size();
        }
    };
    replaceAll(text, "%main_focus", mainFocus);
    replaceAll(text, "%current_goal", currentGoal);
    return text;
}

#endif // MOD_HS_CORPUS_H
