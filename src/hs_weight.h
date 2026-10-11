#ifndef MOD_HS_WEIGHT_H
#define MOD_HS_WEIGHT_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// The weighting both arbiters use to pick who answers: hs_arbiter.cpp for a
// line someone said, hs_event_arbiter.cpp for something that happened.
// Header-only and free of AzerothCore headers, so the event arbiter's
// standalone harness still builds.

// A bot that spoke in the last minute is down-weighted (0.15x at zero
// seconds, back to 1x at sixty), so one bot doesn't answer three lines running.
inline double Hs_RecencyWeight(uint32_t secondsSinceLastReply)
{
    constexpr uint32_t window    = 60;
    constexpr double   minFactor = 0.15;
    if (secondsSinceLastReply >= window)
        return 1.0;

    double t = static_cast<double>(secondsSinceLastReply) / static_cast<double>(window);
    return minFactor + (1.0 - minFactor) * t;
}

// Closer is likelier: say range minus distance, floored at 1 so a bot at the
// edge of range still has a chance. Callers pass 1.0 outright for a bot on
// another map, where a distance means nothing.
inline double Hs_ProximityWeight(float sayDistance, float distance)
{
    return static_cast<double>(std::max(1.0f, sayDistance - distance));
}

// The index `roll` lands on, for a roll in [0, sum of weights). The last
// index if floating-point rounding leaves the roll at the very top.
inline size_t Hs_WeightedIndex(std::vector<double> const& weights, double roll)
{
    double cumulative = 0.0;
    for (size_t i = 0; i < weights.size(); ++i)
    {
        cumulative += weights[i];
        if (roll < cumulative)
            return i;
    }
    return weights.empty() ? 0 : weights.size() - 1;
}

#endif // MOD_HS_WEIGHT_H
