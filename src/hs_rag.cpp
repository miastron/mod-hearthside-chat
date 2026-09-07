#include "hs_rag.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace
{
    // Field weights. Title outranks keywords, which is the opposite of the
    // obvious ordering and was measured, not assumed: the authored keyword
    // arrays list every topic an entry *mentions*, so "blacksmithing" is a
    // keyword of eleven entries and keyword weight alone cannot separate
    // Blacksmithing from Gold Making Methods. The title is the only field
    // that says what an entry is *about*.
    constexpr float kWeightTitle   = 1.00f;
    constexpr float kWeightKeyword = 0.80f;
    constexpr float kWeightContent = 0.50f;

    // Aboutness bonus, on top of coverage: how much of this entry's title the
    // player actually asked for. idf-weighted rather than a plain term count,
    // so matching "Warlock" of "Warlock Class" counts for nearly the whole
    // title while matching it in "Best Professions for Warlocks" does not.
    constexpr float kTitleBonus = 0.35f;

    // A multi-word authored keyword appearing verbatim in the query ("cleft of
    // shadow", "mine cart") is real evidence, but small: kept well under
    // kTitleBonus after an earlier, larger value saturated the score and
    // collapsed the ranking into alphabetical ties.
    constexpr float kPhraseBonus    = 0.08f;
    constexpr float kPhraseBonusCap = 0.16f;

    // Specificity: what share of *this entry's own* retrieval handles (its
    // keywords and title) the query hit. The inverse of the promiscuity
    // problem above -- "dark portal" is two of eight handles on Blasted Lands
    // but two of fifty on Lore Quests, so it is central to one and incidental
    // to the other. Small, because its job is to settle near-ties between
    // entries with equal coverage, not to outvote coverage itself.
    constexpr float kSpecificityBonus = 0.20f;

    // Question scaffolding and chat filler. These appear in nearly every
    // message, carry no retrieval signal, and would otherwise dominate the
    // query's information mass and drag every score toward zero.
    bool IsStopword(const std::string& t)
    {
        static const std::unordered_set<std::string> kStop = {
            "a", "about", "all", "am", "an", "and", "any", "anyone", "are", "as", "at",
            "be", "been", "best", "but", "by", "can", "could", "did", "do", "does",
            "doing", "for", "from", "get", "getting", "go", "going", "good", "got",
            "has", "have", "how", "i", "if", "im", "in", "is", "it", "its", "ive",
            "just", "know", "like", "me", "much", "my", "need", "of", "on", "one",
            "or", "should", "so", "some", "that", "the", "them", "then", "there",
            "they", "this", "to", "too", "up", "us", "use", "very", "was", "we",
            "what", "whats", "when", "where", "which", "who", "why", "will", "with",
            "would", "you", "your", "youre", "u", "r", "ur", "pls", "plz", "thx"
        };
        return kStop.count(t) != 0;
    }

    // Plural folding only. Questions are plural ("what dungeons"), authored
    // keywords are singular ("dungeon"); without this the two never meet.
    // Anything more aggressive (an -ing/-ed stemmer) starts mangling the
    // proper nouns that carry most of the signal here.
    std::string Stem(std::string t)
    {
        if (t.size() < 4 || t.back() != 's')
            return t;

        // bosses -> boss, axes -> axe: only after a sibilant, so zones -> zone
        // still falls through to the plain -s rule below.
        if (t.size() >= 5 && t[t.size() - 2] == 'e')
        {
            char c = t[t.size() - 3];
            if (c == 's' || c == 'x' || c == 'z' || c == 'h' || c == 'c')
            {
                t.erase(t.size() - 2);
                return t;
            }
        }

        std::string tail = t.substr(t.size() - 2);
        if (tail != "ss" && tail != "us" && tail != "is")
            t.pop_back();

        return t;
    }

    std::string Normalize(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        bool lastWasSpace = true;

        for (unsigned char c : s)
        {
            if (std::isalnum(c))
            {
                out.push_back(static_cast<char>(std::tolower(c)));
                lastWasSpace = false;
            }
            else if (!lastWasSpace)
            {
                out.push_back(' ');
                lastWasSpace = true;
            }
        }

        if (!out.empty() && out.back() == ' ')
            out.pop_back();

        return out;
    }

    // Normalized text -> stemmed content terms, stopwords dropped.
    std::vector<std::string> Terms(const std::string& normalized)
    {
        std::vector<std::string> out;
        size_t i = 0;

        while (i < normalized.size())
        {
            size_t j = normalized.find(' ', i);
            if (j == std::string::npos)
                j = normalized.size();

            std::string tok = normalized.substr(i, j - i);
            if (!tok.empty() && !IsStopword(tok))
                out.push_back(Stem(tok));

            i = j + 1;
        }

        return out;
    }

    struct RagIndex
    {
        std::vector<HsRagEntry> entries;

        // term -> (entry index, best field weight for that term in that entry).
        // Best rather than summed: a term repeated ten times in one paragraph
        // is not ten times the evidence, and summing lets the longest entry
        // win every query.
        std::unordered_map<std::string, std::vector<std::pair<uint32_t, float>>> postings;

        // term -> inverse document frequency. Kills the terms every entry
        // shares ("player", "zone", "wow") without a hand-maintained list.
        std::unordered_map<std::string, float> idf;

        // Weight for a query term the corpus has never seen. See IdfFor.
        float unknownIdf = 0.0f;

        // Multi-word authored keywords, normalized, per entry.
        std::vector<std::vector<std::string>> phrases;

        // Stemmed title terms per entry, for the aboutness bonus.
        std::vector<std::vector<std::string>> titleTerms;

        // Distinct keyword/title terms per entry: the denominator of the
        // specificity bonus.
        std::vector<uint32_t> handleCount;
    };

    RagIndex& Index()
    {
        static RagIndex idx;
        return idx;
    }

    void AccumulateBest(std::unordered_map<std::string, float>& best,
                        const std::vector<std::string>& terms, float weight)
    {
        for (const std::string& t : terms)
        {
            auto it = best.find(t);
            if (it == best.end() || it->second < weight)
                best[t] = weight;
        }
    }

    // A term the corpus has never seen is usually just vocabulary the entries
    // phrase differently ("train" vs "trainer"), not evidence the question is
    // off-topic -- off-topic is rejected by scoring zero coverage, not by this
    // denominator. Charging such a term the maximum idf therefore punished
    // answerable questions hardest: "where do i train blacksmithing" scored
    // 0.35 because one unmatched word carried 65% of the query mass. The
    // corpus mean is the honest weight for "no information either way".
    float IdfFor(const RagIndex& idx, const std::string& term)
    {
        auto it = idx.idf.find(term);
        return it != idx.idf.end() ? it->second : idx.unknownIdf;
    }
}

void Hs_SetRagTable(const std::vector<HsRagEntry>& rows)
{
    RagIndex& idx = Index();
    idx.entries = rows;
    idx.postings.clear();
    idx.idf.clear();
    idx.phrases.assign(rows.size(), {});
    idx.titleTerms.assign(rows.size(), {});
    idx.handleCount.assign(rows.size(), 0);

    std::unordered_map<std::string, uint32_t> docFreq;

    for (uint32_t i = 0; i < idx.entries.size(); ++i)
    {
        const HsRagEntry& e = idx.entries[i];
        std::unordered_map<std::string, float> best;

        AccumulateBest(best, Terms(Normalize(e.content)), kWeightContent);

        for (const std::string& kw : e.keywords)
        {
            std::string norm = Normalize(kw);
            AccumulateBest(best, Terms(norm), kWeightKeyword);

            if (norm.find(' ') != std::string::npos)
                idx.phrases[i].push_back(norm);
        }

        idx.titleTerms[i] = Terms(Normalize(e.title));
        AccumulateBest(best, idx.titleTerms[i], kWeightTitle);

        for (const auto& kv : best)
        {
            idx.postings[kv.first].push_back({ i, kv.second });
            docFreq[kv.first]++;

            // A term reaching keyword weight or better came from `keywords` or
            // the title, not from prose: those are the entry's handles.
            if (kv.second >= kWeightKeyword)
                idx.handleCount[i]++;
        }
    }

    const float n = static_cast<float>(idx.entries.size());
    float idfSum = 0.0f;
    for (const auto& kv : docFreq)
    {
        const float v = std::log(1.0f + n / static_cast<float>(kv.second));
        idx.idf[kv.first] = v;
        idfSum += v;
    }

    idx.unknownIdf = idx.idf.empty() ? 0.0f : idfSum / static_cast<float>(idx.idf.size());
}

size_t Hs_RagEntryCount()
{
    return Index().entries.size();
}

std::vector<HsRagHit> Hs_RetrieveRag(const std::string& query, uint32_t maxEntries, float minScore)
{
    std::vector<HsRagHit> hits;

    const RagIndex& idx = Index();
    if (idx.entries.empty() || maxEntries == 0)
        return hits;

    const std::string normalizedQuery = Normalize(query);

    // Dedupe: a term repeated in the question should not count twice toward
    // either the numerator or the denominator.
    std::vector<std::string> queryTerms;
    for (const std::string& t : Terms(normalizedQuery))
    {
        if (std::find(queryTerms.begin(), queryTerms.end(), t) == queryTerms.end())
            queryTerms.push_back(t);
    }

    if (queryTerms.empty())
        return hits;

    // The denominator: everything the player asked about, weighted by how
    // discriminating each term is. Scoring against this rather than against
    // the entry's own length is the whole fix.
    float queryMass = 0.0f;
    for (const std::string& t : queryTerms)
        queryMass += IdfFor(idx, t);

    if (queryMass <= 0.0f)
        return hits;

    std::unordered_map<uint32_t, float>    covered;
    std::unordered_map<uint32_t, uint32_t> handleHits;

    for (const std::string& t : queryTerms)
    {
        auto post = idx.postings.find(t);
        if (post == idx.postings.end())
            continue;

        const float termIdf = IdfFor(idx, t);
        for (const auto& entryWeight : post->second)
        {
            covered[entryWeight.first] += termIdf * entryWeight.second;

            if (entryWeight.second >= kWeightKeyword)
                handleHits[entryWeight.first]++;
        }
    }

    for (const auto& kv : covered)
    {
        float score = kv.second / queryMass;

        // Aboutness: how much of this entry's title did the player ask for.
        const std::vector<std::string>& title = idx.titleTerms[kv.first];
        float titleMass = 0.0f;
        float titleHit  = 0.0f;
        for (const std::string& t : title)
        {
            const float w = IdfFor(idx, t);
            titleMass += w;
            if (std::find(queryTerms.begin(), queryTerms.end(), t) != queryTerms.end())
                titleHit += w;
        }

        if (titleMass > 0.0f && titleHit > 0.0f)
            score += kTitleBonus * (titleHit / titleMass);

        uint32_t phraseHits = 0;
        for (const std::string& phrase : idx.phrases[kv.first])
        {
            if (normalizedQuery.find(phrase) != std::string::npos)
                phraseHits++;
        }

        score += std::min(kPhraseBonusCap, kPhraseBonus * static_cast<float>(phraseHits));

        // Specificity: how central this query is to the entry's own handles.
        const uint32_t handles = idx.handleCount[kv.first];
        auto           hit     = handleHits.find(kv.first);
        if (handles > 0 && hit != handleHits.end())
            score += kSpecificityBonus * (static_cast<float>(hit->second) / static_cast<float>(handles));

        if (score >= minScore)
            hits.push_back({ &idx.entries[kv.first], score });
    }

    std::sort(hits.begin(), hits.end(), [](const HsRagHit& a, const HsRagHit& b) {
        if (a.score != b.score)
            return a.score > b.score;
        return a.entry->id < b.entry->id;
    });

    if (hits.size() > maxEntries)
        hits.resize(maxEntries);

    return hits;
}

std::string Hs_RagContextLine(const std::vector<HsRagHit>& hits, uint32_t maxChars)
{
    if (hits.empty() || maxChars == 0)
        return "";

    const std::string prefix = "Things you know about Azeroth: ";
    std::string line = prefix;

    for (size_t i = 0; i < hits.size(); ++i)
    {
        std::string piece = hits[i].entry->title + " -- " + hits[i].entry->content;
        if (i > 0)
            piece = " " + piece;

        if (line.size() + piece.size() <= maxChars)
        {
            line += piece;
            continue;
        }

        // Only the first entry is worth truncating mid-way; past that,
        // dropping the weaker hit reads better than a severed sentence.
        if (i > 0)
            break;

        if (maxChars <= prefix.size() + 4)
            return "";

        size_t room = maxChars - prefix.size() - 3;
        size_t cut  = piece.rfind(' ', room);
        if (cut == std::string::npos || cut == 0)
            cut = room;

        line += piece.substr(0, cut) + "...";
        break;
    }

    return line == prefix ? "" : line;
}
