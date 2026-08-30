#include "hs_grounded.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <vector>

namespace
{
    // Same SplitMix64 finalizer hs_style.cpp/hs_archetype.cpp/hs_reflex.cpp
    // use, for the same reason: AzerothCore GUIDs are sequential, so
    // std::hash<uint64_t> alone barely perturbs neighbouring GUIDs.
    // Duplicated locally rather than shared, matching this module's
    // existing per-file precedent.
    uint64_t MixBits64(uint64_t x)
    {
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBULL;
        x ^= x >> 31;
        return x;
    }

    // hash(botGuid, message text). Seeded per message rather than per
    // bot, same idiom as hs_reflex.cpp's SeedForMessage (Plain family).
    uint64_t SeedForMessage(uint64_t botGuid, const std::string& text)
    {
        uint64_t h = std::hash<std::string>{}(text);
        h ^= MixBits64(botGuid) + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        return h;
    }

    std::string ToLowerAscii(const std::string& s)
    {
        std::string out = s;
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    std::string NormalizeWhitespace(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        bool lastWasSpace = true;
        for (char c : s)
        {
            if (std::isspace(static_cast<unsigned char>(c)))
            {
                if (!lastWasSpace)
                    out.push_back(' ');
                lastWasSpace = true;
            }
            else
            {
                out.push_back(c);
                lastWasSpace = false;
            }
        }
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    }

    std::string StripOneTrailingMark(const std::string& s)
    {
        if (!s.empty())
        {
            char last = s.back();
            if (last == '?' || last == '!' || last == '.')
                return s.substr(0, s.size() - 1);
        }
        return s;
    }

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
}

void Hs_SetGroundedQuestionTable(const std::vector<HsGroundedQuestionRow>& rows)
{
    g_Questions = rows;
}

void Hs_SetGroundedTemplateTable(const std::vector<HsGroundedTemplateRow>& rows)
{
    g_Templates = rows;
}

HsGroundedKind Hs_MatchGroundedQuestion(const std::string& trigger, uint32_t fuzzyMaxDistance)
{
    std::string corePhrase = StripOneTrailingMark(NormalizeWhitespace(ToLowerAscii(trigger)));

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

    std::vector<const HsGroundedTemplateRow*> matches;
    for (auto const& t : g_Templates)
        if (t.kind == kind && t.hasFact == hasFact)
            matches.push_back(&t);

    if (matches.empty())
        return "";

    uint64_t seed = SeedForMessage(botGuid, trigger);
    const HsGroundedTemplateRow& t = *matches[seed % matches.size()];
    return t.usesFact ? (t.prefix + fact + t.suffix) : t.prefix;
}
