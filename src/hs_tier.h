#ifndef MOD_HS_TIER_H
#define MOD_HS_TIER_H

#include <string>

// PLAN.md §4.14: one enum, five keys, one shared parse and resolve helper.
// Ordered — reflex < corpus < inference — so a surface's config value is a
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

// True if the ceiling permits at least the given tier.
inline bool HsTierAllows(HsTier ceiling, HsTier requested)
{
    return ceiling != HsTier::Off && static_cast<int>(ceiling) >= static_cast<int>(requested);
}

#endif // MOD_HS_TIER_H
