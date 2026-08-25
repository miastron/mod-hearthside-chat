#include "hs_event_arbiter.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <random>

namespace
{
    // Indexed by HsEventType's underlying value -- declaration order in the
    // enum and entry order here are one contract, not two lists that happen
    // to agree.
    constexpr const char* kEventTypeNames[kHsEventTypeCount] = {
        "DEATH_IN_GROUP",
        "DEATH_WIPE",
        "DEATH_GROUP_PLAYER",
        "DEATH_SOLO",
        "LEVEL_UP_SELF",
        "LEVEL_UP_GROUP",
        "KILLING_BLOW",
        "ROLL_WON",
        "ROLL_LOST",
        "DUEL_START",
        "DUEL_WON",
        "DUEL_LOST",
        "OPENER_GROUP_FORMED",
        "OPENER_REZ",
        "OPENER_DUNGEON_COMPLETE",
        "OPENER_PROXIMITY",
    };

    // { chance of nobody, chance of exactly one }; the remainder is the
    // chance of two. Starting guesses shaped by PLAN-ARBITER.md §1/§2's
    // three stated judgements -- most deaths pass without comment, a ding
    // almost always draws a "gz", duels draw little beyond the occasional
    // jab -- not measurements. Same status as hs_opener.cpp's fire-chance
    // constants: a live-realm judgement, retuned by editing this table.
    constexpr HsEventCountBias kEventCountBias[kHsEventTypeCount] = {
        { 70, 28 }, // DEATH_IN_GROUP          -- one death mid-fight is unremarkable
        { 50, 42 }, // DEATH_WIPE              -- a wipe is the one death people do talk about
        { 65, 33 }, // DEATH_GROUP_PLAYER      -- someone says something to the person who died
        { 80, 20 }, // DEATH_SOLO              -- nobody to say it to; mostly silence
        { 55, 43 }, // LEVEL_UP_SELF           -- announcing your own ding, not congratulating
        { 20, 68 }, // LEVEL_UP_GROUP          -- a ding almost always draws a gz (§1)
        { 60, 38 }, // KILLING_BLOW
        { 45, 52 }, // ROLL_WON                -- winners say something more often than not
        { 65, 33 }, // ROLL_LOST
        { 80, 20 }, // DUEL_START              -- heavy bias toward 0 (§2, operator read)
        { 75, 25 }, // DUEL_WON
        { 75, 25 }, // DUEL_LOST
        { 40, 55 }, // OPENER_GROUP_FORMED     -- reserved; openers are tier-1 corpus today
        { 55, 45 }, // OPENER_REZ
        { 45, 50 }, // OPENER_DUNGEON_COMPLETE
        { 65, 35 }, // OPENER_PROXIMITY
    };

    // Involvement multipliers on the shared three-level scale. Wide enough
    // that the bot something happened *to* usually speaks over a bystander,
    // narrow enough that affinity and recency can still overturn it -- a
    // subject that answered ten seconds ago (0.15x recency) loses to a
    // witness that has been quiet.
    double InvolvementWeight(HsEventInvolvement involvement)
    {
        switch (involvement)
        {
            case HsEventInvolvement::Subject:  return 4.0;
            case HsEventInvolvement::Affected: return 2.0;
            case HsEventInvolvement::Witness:  return 1.0;
        }
        return 1.0;
    }

    // Byte-identical curve to hs_arbiter.cpp's RecencyWeight, duplicated
    // rather than shared because that file is not dependency-free (Player.h)
    // and hoisting it here would mean including this header there, dragging
    // the event vocabulary into the /say path for one function. Both are
    // eleven lines; a shared "weights" file for two would be the heavier
    // change.
    double RecencyWeight(uint32_t secondsSinceLastReply)
    {
        constexpr uint32_t window    = 60;
        constexpr double   minFactor = 0.15;
        if (secondsSinceLastReply >= window)
            return 1.0;

        double t = static_cast<double>(secondsSinceLastReply) / static_cast<double>(window);
        return minFactor + (1.0 - minFactor) * t;
    }

    // Same falloff and same beyond-range floor as the /say arbiter's, with
    // the cross-map case decided by the caller's sameMap flag instead of a
    // GetMapId() comparison this file cannot make.
    double ProximityWeight(float sayDistance, const HsEventCandidate& candidate)
    {
        if (!candidate.sameMap)
            return 1.0;

        float falloff = sayDistance - candidate.distance;
        return static_cast<double>(std::max(1.0f, falloff));
    }

    // ---- affinity table ----------------------------------------------------
    // Replaced wholesale on every load (startup and `.reload config`), read
    // from the world thread at each fire site. The mutex is what makes the
    // replace safe against a read in progress; contention is irrelevant --
    // reads happen once per candidate per event, writes twice a session.
    std::mutex                                        g_AffinityMutex;
    std::map<std::pair<uint8_t, std::string>, float>  g_Affinity;

    // Seeded once from random_device, then reused -- constructing a
    // random_device per event would be both slower and, on some libstdc++
    // builds, a repeated open of /dev/urandom. Hs_SeedEventArbiterForTest
    // overwrites it for the harness.
    std::mt19937& Rng()
    {
        static std::mt19937 gen{ std::random_device{}() };
        return gen;
    }

    uint32_t Roll100()
    {
        std::uniform_int_distribution<uint32_t> dist(0, 99);
        return dist(Rng());
    }
}

const char* Hs_EventTypeName(HsEventType type)
{
    size_t index = static_cast<size_t>(type);
    return index < kHsEventTypeCount ? kEventTypeNames[index] : "UNKNOWN";
}

bool Hs_EventTypeForName(const std::string& name, HsEventType& out)
{
    for (size_t i = 0; i < kHsEventTypeCount; ++i)
    {
        if (name == kEventTypeNames[i])
        {
            out = static_cast<HsEventType>(i);
            return true;
        }
    }
    return false;
}

HsEventCountBias Hs_EventCountBiasFor(HsEventType type)
{
    size_t index = static_cast<size_t>(type);
    return index < kHsEventTypeCount ? kEventCountBias[index] : HsEventCountBias{ 50, 42 };
}

void Hs_SetEventAffinityTable(const std::vector<HsEventAffinityRow>& rows)
{
    std::map<std::pair<uint8_t, std::string>, float> table;
    for (auto const& row : rows)
        table[{ static_cast<uint8_t>(row.type), row.archetypeName }] = row.weight;

    std::lock_guard<std::mutex> lock(g_AffinityMutex);
    g_Affinity.swap(table);
}

float Hs_EventAffinityWeight(HsEventType type, const std::string& archetypeName)
{
    std::lock_guard<std::mutex> lock(g_AffinityMutex);
    auto it = g_Affinity.find({ static_cast<uint8_t>(type), archetypeName });
    // Default 1.0, so the SQL only has to author the exceptions -- an
    // archetype with no row for an event is neither favoured nor penalised.
    return it == g_Affinity.end() ? 1.0f : it->second;
}

std::vector<size_t> Hs_ArbitrateEventReplies(HsEventCountBias bias, float sayDistance,
                                              const std::vector<HsEventCandidate>& candidates)
{
    std::vector<size_t> selected;
    if (candidates.empty())
        return selected;

    uint32_t roll  = Roll100();
    uint32_t count = (roll < bias.none) ? 0u
                    : (roll < static_cast<uint32_t>(bias.none) + bias.one) ? 1u : 2u;
    count = static_cast<uint32_t>(std::min<size_t>(count, candidates.size()));
    if (count == 0)
        return selected;

    // Weighted select without replacement over indices, so the caller can
    // pair each pick back to its own trigger text.
    std::vector<size_t> pool;
    pool.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i)
        pool.push_back(i);

    for (uint32_t picked = 0; picked < count && !pool.empty(); ++picked)
    {
        std::vector<double> weights;
        weights.reserve(pool.size());
        double total = 0.0;
        for (size_t index : pool)
        {
            HsEventCandidate const& c = candidates[index];
            double w = ProximityWeight(sayDistance, c)
                      * RecencyWeight(c.secondsSinceLastReply)
                      * InvolvementWeight(c.involvement)
                      * static_cast<double>(c.affinityWeight);
            weights.push_back(w);
            total += w;
        }

        // Every remaining candidate is affinity-zeroed ("never speaks to
        // this event"). Stop rather than fall through to the last-index
        // default below, which would hand the reply to a bot the table
        // explicitly silenced.
        if (total <= 0.0)
            break;

        std::uniform_real_distribution<double> dist(0.0, total);
        double rollWeight = dist(Rng());
        size_t pickSlot   = pool.size() - 1;
        double cumulative = 0.0;
        for (size_t slot = 0; slot < weights.size(); ++slot)
        {
            cumulative += weights[slot];
            if (rollWeight < cumulative)
            {
                pickSlot = slot;
                break;
            }
        }

        selected.push_back(pool[pickSlot]);
        pool.erase(pool.begin() + static_cast<long>(pickSlot));
    }

    return selected;
}

void Hs_SeedEventArbiterForTest(uint32_t seed)
{
    Rng().seed(seed);
}
