#ifndef MOD_HS_TIER_H
#define MOD_HS_TIER_H

#include <string>

// One enum, seven keys, one shared parse and resolve helper. Ordered
// reflex < corpus < inference, so a surface's config value is a
// ceiling, not a mode switch.
enum class HsTier
{
    Off,
    Reflex,
    Corpus,
    Inference,
};

inline HsTier HsParseTier(const std::string& value)
{
    if (value == "inference") return HsTier::Inference;
    if (value == "corpus")    return HsTier::Corpus;
    if (value == "reflex")    return HsTier::Reflex;
    return HsTier::Off;
}

// The config spelling of a tier: the exact string HsParseTier accepts, so a
// log line naming a tier reads back as something the operator can paste
// into a config key. Added for review H3's Channel.MaxCandidates warning.
inline const char* HsTierName(HsTier tier)
{
    switch (tier)
    {
        case HsTier::Inference: return "inference";
        case HsTier::Corpus:    return "corpus";
        case HsTier::Reflex:    return "reflex";
        case HsTier::Off:
        default:                 return "off";
    }
}

// True if the ceiling permits at least the given tier.
inline bool HsTierAllows(HsTier ceiling, HsTier requested)
{
    return ceiling != HsTier::Off && static_cast<int>(ceiling) >= static_cast<int>(requested);
}

#endif // MOD_HS_TIER_H
