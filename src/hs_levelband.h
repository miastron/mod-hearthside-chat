#ifndef MOD_HS_LEVELBAND_H
#define MOD_HS_LEVELBAND_H

#include <cstdint>
#include <string>

// The four level bands, and the three numbers that define them.
//
// Lined up with WotLK's own leveling pace (Outland opens at 58, Northrend at
// 68, raiding and dailies only exist at the level-80 cap) rather than an even
// split: low 1-19, mid 20-59, high 60-79, endgame 80.
//
// A header of its own because two files need the same boundaries and only one
// of them can include hs_corpus.h, where this used to live (review item 25).
// hs_identity.cpp's card-fact plausibility check kept its own copy of 20/60/80
// with a comment saying the two had to match -- a rebalancing would have had
// to be applied twice by hand, with nothing to catch a mismatch. hs_corpus.h
// includes this header, so every existing caller of Hs_LevelBandFor is
// unaffected.
//
// Inline and dependency-free (no AzerothCore, nothing that pulls in
// DatabaseEnv.h) so the standalone Tests/ harnesses on either side keep
// building, same pattern as hs_tier.h's HsParseTier/HsTierAllows.
constexpr uint8_t kHsLevelBandMidMin     = 20;
constexpr uint8_t kHsLevelBandHighMin    = 60;
constexpr uint8_t kHsLevelBandEndgameMin = 80;

// The label form, as used by chat_levelband_musing's level_band_tag rows.
inline std::string Hs_LevelBandFor(uint8_t level)
{
    if (level >= kHsLevelBandEndgameMin) return "endgame";
    if (level >= kHsLevelBandHighMin)    return "high";
    if (level >= kHsLevelBandMidMin)     return "mid";
    return "low";
}

#endif // MOD_HS_LEVELBAND_H
