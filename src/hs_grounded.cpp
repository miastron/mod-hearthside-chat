#include "hs_grounded.h"
#include "hs_hash.h"
#include "hs_text.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <mutex>
#include <vector>

namespace
{
    // Iterative two-row Levenshtein distance, no recursion, no library.
    // Only ever called on short chat phrases (a handful of words), so the
    // O(len(a) * len(b)) cost is negligible; Hs_MatchGroundedQuestion also
    // skips a candidate outright when its length differs from the trigger
    // by more than the caller's distance cap, so this rarely runs at all.
    uint32_t LevenshteinDistance(const std::string& a, const std::string& b)
    {
        std::vector<uint32_t> prev(b.size() + 1), curr(b.size() + 1);
        for (size_t j = 0; j <= b.size(); ++j)
            prev[j] = static_cast<uint32_t>(j);

        for (size_t i = 1; i <= a.size(); ++i)
        {
            curr[0] = static_cast<uint32_t>(i);
            for (size_t j = 1; j <= b.size(); ++j)
            {
                uint32_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
                curr[j] = std::min({ prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost });
            }
            std::swap(prev, curr);
        }
        return prev[b.size()];
    }

    std::vector<HsGroundedQuestionRow> g_Questions;
    std::vector<HsGroundedTemplateRow> g_Templates;

    // Review B6: guards both tables. Every row owns std::strings, and
    // `.reload config` replaces the whole vector (hs_main.cpp's
    // HsGroundedLifecycleWorldScript), so an unguarded replace frees
    // buffers a reader may be mid-iteration on -- the same hazard, and the
    // same fix, hs_archetype.cpp applies to g_Archetypes and hs_config.h
    // documents for the config strings. Both readers are world-thread-only
    // today, so this was latent; the point is that the module's rule is
    // "owned strings get a lock, scalars don't" (hs_config.h) and these two
    // were the exception to it. hs_channel.cpp's g_ChannelPolicies stays
    // lock-free and is correct to: HsChannelPolicy is an enum plus two
    // uint32_t with nothing owned, so the worst a torn read can do is give
    // one message a wrong number.
    std::mutex g_GroundedTableMutex;
}

void Hs_SetGroundedQuestionTable(const std::vector<HsGroundedQuestionRow>& rows)
{
    std::lock_guard<std::mutex> lock(g_GroundedTableMutex);
    g_Questions = rows;
}

void Hs_SetGroundedTemplateTable(const std::vector<HsGroundedTemplateRow>& rows)
{
    std::lock_guard<std::mutex> lock(g_GroundedTableMutex);
    g_Templates = rows;
}

HsGroundedKind Hs_MatchGroundedQuestion(const std::string& trigger, uint32_t fuzzyMaxDistance)
{
    std::string corePhrase = HsText::Hs_StripOneTrailingMark(HsText::Hs_NormalizeWhitespace(HsText::Hs_ToLowerAscii(trigger)));

    // Held for the whole scan (review B6): the loops read q.phrase by
    // reference. Returns an enum, so nothing outlives the lock.
    std::lock_guard<std::mutex> lock(g_GroundedTableMutex);

    for (auto const& q : g_Questions)
        if (corePhrase == q.phrase)
            return q.kind;

    if (fuzzyMaxDistance == 0)
        return HsGroundedKind::None;

    // Typo-tolerance fallback: closest phrase within fuzzyMaxDistance wins;
    // a tie between two different kinds is ambiguous, not a guess.
    HsGroundedKind best         = HsGroundedKind::None;
    uint32_t       bestDistance = fuzzyMaxDistance + 1;
    bool           ambiguous    = false;

    for (auto const& q : g_Questions)
    {
        size_t lenDiff = corePhrase.size() > q.phrase.size()
                              ? corePhrase.size() - q.phrase.size()
                              : q.phrase.size() - corePhrase.size();
        if (lenDiff > fuzzyMaxDistance)
            continue;

        uint32_t d = LevenshteinDistance(corePhrase, q.phrase);
        if (d > fuzzyMaxDistance)
            continue;

        if (d < bestDistance)
        {
            bestDistance = d;
            best         = q.kind;
            ambiguous    = false;
        }
        else if (d == bestDistance && q.kind != best)
        {
            ambiguous = true;
        }
    }

    return ambiguous ? HsGroundedKind::None : best;
}

std::string Hs_BuildGroundedReply(HsGroundedKind kind, bool hasFact, const std::string& fact,
                                    uint64_t botGuid, const std::string& trigger)
{
    if (kind == HsGroundedKind::None)
        return "";

    // Held past the pick (review B6): `matches` holds pointers into
    // g_Templates and the return value copies out of the row's strings.
    std::lock_guard<std::mutex> lock(g_GroundedTableMutex);

    std::vector<const HsGroundedTemplateRow*> matches;
    for (auto const& t : g_Templates)
        if (t.kind == kind && t.hasFact == hasFact)
            matches.push_back(&t);

    if (matches.empty())
        return "";

    uint64_t seed = HsHash::Hs_SeedForMessage(botGuid, trigger);
    const HsGroundedTemplateRow& t = *matches[seed % matches.size()];
    return t.usesFact ? (t.prefix + fact + t.suffix) : t.prefix;
}
