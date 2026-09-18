#include "hs_rag.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <shared_mutex>
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

    // Terms that cannot, on their own, identify one entry. Two different
    // things land in here and the gate below treats them the same way:
    //
    //   - ordinary English that is also a retrieval handle ("holy", "arms",
    //     "fire", "down"). Corpus idf cannot find these -- it measures rarity
    //     *in this corpus*, not in English, and "blacksmithing" and "holy"
    //     sit at the same idf here. So it has to be authored.
    //   - spec words and abbreviations shared by more than one class
    //     ("frost" is mage and death knight, "restoration" is druid and
    //     shaman, "prot" is warrior and paladin). Retrieving one of them on
    //     a coin flip is worse than retrieving nothing.
    //
    // Being listed here costs an entry nothing when the query also carries a
    // real handle: "ret pally", "frost mage", "bm hunter" all still resolve,
    // because the class word is unambiguous. It only bites when such a term
    // is the *only* thing that matched, which is the measured signature of a
    // plain-English collision:
    //
    //     "holy crap that was close"                    -> Holy Paladin 0.51
    //     "my arms are killing me"                      -> Arms Warrior 0.81
    //     "there was a fire down the street last night" -> Razorfen Downs 0.50
    //
    // Grow this list when a collision is *measured*, not when one is imagined:
    // a word added here stops being able to answer on its own. The generic
    // fantasy nouns ("light", "storm", "dark") were tried and left off for
    // exactly that reason -- none of them produced a false positive, and
    // "where is the dark portal" needs "dark" to keep pulling its weight.
    bool IsAmbiguousTerm(const std::string& t)
    {
        static const std::unordered_set<std::string> kAmbiguous = []{
            // Stemmed on the way in, because the scorer stems query terms:
            // "arms" reaches the gate as "arm", and a raw list never matches.
            static const char* const kRaw[] = {
                // spec and role words that are also everyday English
                "arms", "fury", "fire", "frost", "holy", "shadow", "blood",
                "combat", "balance", "protection", "restoration", "discipline",
                "survival", "beast", "mastery", "arcane", "feral", "elemental",
                "cat", "bear",
                // spec abbreviations: too short to be evidence by themselves,
                // and "bm" is Beast Mastery *and* Black Morass
                "prot", "ret", "resto", "disc", "sub", "demo", "bm", "mm",
                "sv", "ele", "enh", "affli", "destro",
                // measured collision: "down the street" -> Razorfen Downs
                "down", "downs"
            };

            std::unordered_set<std::string> out;
            for (const char* w : kRaw)
                out.insert(Stem(w));

            return out;
        }();

        return kAmbiguous.count(t) != 0;
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

        // Direct-address index for Hs_RagContextForKeys: entry id (verbatim)
        // and normalized title, both mapping to the entry's slot. Built here
        // rather than scanned per lookup because the keyed path is the one
        // the generator hits once per bucket per cycle.
        std::unordered_map<std::string, uint32_t> byKey;
    };

    RagIndex& Index()
    {
        static RagIndex idx;
        return idx;
    }

    // Guards RagIndex against a `.reload config` on the world thread landing
    // while the queue worker or the generator is mid-retrieval. Shared: reads
    // are frequent and concurrent, the swap is once at startup and once per
    // reload.
    //
    // std::shared_mutex is not recursive even for shared ownership, so every
    // helper below that runs under it is named *Locked and takes no lock of
    // its own; only the public entry points acquire.
    std::shared_mutex& TableMutex()
    {
        static std::shared_mutex m;
        return m;
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
    std::unique_lock<std::shared_mutex> guard(TableMutex());

    RagIndex& idx = Index();
    idx.entries = rows;
    idx.postings.clear();
    idx.idf.clear();
    idx.byKey.clear();
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

        // emplace, not []: on a duplicate id or two entries sharing a title
        // the first-loaded wins, deterministically, rather than the last row
        // the query happened to return. data/rag/README.md already warns that
        // nothing enforces id uniqueness at load time.
        idx.byKey.emplace(e.id, i);
        std::string normTitle = Normalize(e.title);
        if (!normTitle.empty())
            idx.byKey.emplace(normTitle, i);

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
    std::shared_lock<std::shared_mutex> guard(TableMutex());
    return Index().entries.size();
}

namespace
{
// The scoring pass, minus the lock. Public entry points below acquire once
// and call this; see TableMutex's note on why nothing here re-acquires.
std::vector<HsRagHit> RetrieveLocked(const std::string& query, uint32_t maxEntries, float minScore)
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

    // The subset of handleHits that is not IsAmbiguousTerm. Drives the
    // eligibility gate below; handleHits itself keeps driving specificity.
    std::unordered_map<uint32_t, uint32_t> unambiguousHandleHits;

    for (const std::string& t : queryTerms)
    {
        auto post = idx.postings.find(t);
        if (post == idx.postings.end())
            continue;

        const float termIdf = IdfFor(idx, t);
        for (const auto& entryWeight : post->second)
        {
            covered[entryWeight.first] += termIdf * entryWeight.second;

            // Handle weight only -- title or keyword. A term that appears
            // merely in an entry's *prose* corroborates a hit but is never
            // evidence that the entry is what was asked about: "fire" reaches
            // Razorfen Downs through "Mordresh Fire Eye" in its content, which
            // is how a sentence about a house fire retrieved a dungeon.
            if (entryWeight.second >= kWeightKeyword)
            {
                handleHits[entryWeight.first]++;

                if (!IsAmbiguousTerm(t))
                    unambiguousHandleHits[entryWeight.first]++;
            }
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

        // Eligibility, which is a separate question from score. Scoring
        // normalizes by query mass, so one matched term in a short sentence
        // scores *high* -- that is the point of the normalization and it is
        // what makes short questions work ("how do i get to dalaran" -> 1.36
        // on a single term). The cost is that one accidental match in an
        // ordinary English sentence scores just as confidently, and no
        // threshold separates the two: measured against this corpus, every
        // false positive matched exactly one handle, while every genuine hit
        // matched either two handles or one unambiguous one.
        //
        // So: an entry is eligible if the query hit a handle that means
        // something on its own, or hit two handles of any kind. The second
        // clause is what keeps "should i go cat or bear" working -- two
        // ambiguous words together are a real signal even though neither is
        // alone. See Tests/test_hs_rag_ambiguity.cpp.
        const uint32_t handleMatches = handleHits.count(kv.first)
                                     ? handleHits[kv.first] : 0u;
        const uint32_t namedMatches  = unambiguousHandleHits.count(kv.first)
                                     ? unambiguousHandleHits[kv.first] : 0u;

        if (namedMatches == 0 && handleMatches < 2)
            continue;

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
} // namespace

std::vector<HsRagHit> Hs_RetrieveRag(const std::string& query, uint32_t maxEntries, float minScore)
{
    std::shared_lock<std::shared_mutex> guard(TableMutex());
    return RetrieveLocked(query, maxEntries, minScore);
}

size_t Hs_RagQueryTermCount(const std::string& query)
{
    // Terms() is the same reduction RetrieveLocked scores through, so this
    // counts precisely what the scorer would see -- not words, and not
    // characters.
    return Terms(Normalize(query)).size();
}

std::string Hs_RagContextFor(const std::string& query, uint32_t maxEntries, float minScore, uint32_t maxChars,
                             const std::string& prefix)
{
    std::shared_lock<std::shared_mutex> guard(TableMutex());
    // Hs_RagContextLine only reads through the hit pointers, which stay valid
    // for as long as this guard is held, so formatting inside the lock is
    // what keeps them from escaping it.
    return Hs_RagContextLine(RetrieveLocked(query, maxEntries, minScore), maxChars, prefix);
}

std::string Hs_RagContextForKeys(const std::vector<std::string>& keys, uint32_t maxChars,
                                 const std::string& prefix)
{
    if (keys.empty() || maxChars == 0)
        return "";

    std::shared_lock<std::shared_mutex> guard(TableMutex());
    const RagIndex& idx = Index();

    for (const std::string& key : keys)
    {
        if (key.empty())
            continue;

        auto it = idx.byKey.find(key); // verbatim id
        if (it == idx.byKey.end())
        {
            std::string norm = Normalize(key); // title, punctuation-insensitive
            if (norm.empty())
                continue;
            it = idx.byKey.find(norm);
            if (it == idx.byKey.end())
                continue;
        }

        // Score 1.0 is cosmetic: nothing downstream reads it on this path,
        // and a keyed hit has no score to report -- it was addressed, not
        // ranked.
        std::vector<HsRagHit> hits{ { &idx.entries[it->second], 1.0f } };
        return Hs_RagContextLine(hits, maxChars, prefix);
    }

    return "";
}

std::string Hs_RagContextRandom(uint32_t selector, uint32_t maxChars, const std::string& prefix)
{
    if (maxChars == 0)
        return "";

    std::shared_lock<std::shared_mutex> guard(TableMutex());
    const RagIndex& idx = Index();

    if (idx.entries.empty())
        return "";

    const size_t pick = static_cast<size_t>(selector) % idx.entries.size();

    // Score 1.0 is cosmetic, same as the keyed path: nothing downstream reads
    // it, and a drawn entry has no score to report.
    std::vector<HsRagHit> hits{ { &idx.entries[pick], 1.0f } };
    return Hs_RagContextLine(hits, maxChars, prefix);
}

std::string Hs_RagBlockLeadTitle(const std::string& block, const std::string& prefix)
{
    if (block.empty() || block.size() <= prefix.size())
        return "";

    if (block.compare(0, prefix.size(), prefix) != 0)
        return "";

    const std::string body = block.substr(prefix.size());
    const size_t      sep  = body.find(" -- ");

    // No separator means the block was truncated inside the title (a very
    // small maxChars); the leading text is still the best label available.
    return sep == std::string::npos ? body.substr(0, 48) : body.substr(0, sep);
}

std::string Hs_RagContextLine(const std::vector<HsRagHit>& hits, uint32_t maxChars, const std::string& prefix)
{
    if (hits.empty() || maxChars == 0)
        return "";

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
