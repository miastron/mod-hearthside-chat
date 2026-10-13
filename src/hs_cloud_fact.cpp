#include "hs_cloud_fact.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace
{
    std::string Lower(const std::string& s)
    {
        std::string out = s;
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    std::string Trim(const std::string& s)
    {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
            ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
            --e;
        return s.substr(b, e - b);
    }

    // Lowercase alphanumeric words; an apostrophe is dropped inside a word
    // ("what's" -> "whats") so it matches how chat spells it without one.
    std::vector<std::string> Words(const std::string& text)
    {
        std::vector<std::string> out;
        std::string cur;
        for (char raw : text)
        {
            unsigned char c = static_cast<unsigned char>(raw);
            if (std::isalnum(c))
                cur += static_cast<char>(std::tolower(c));
            else if (raw == '\'' && !cur.empty())
                continue;
            else if (!cur.empty())
            {
                out.push_back(cur);
                cur.clear();
            }
        }
        if (!cur.empty())
            out.push_back(cur);
        return out;
    }

    bool IsNumber(const std::string& w)
    {
        return !w.empty() && std::all_of(w.begin(), w.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    }
}

bool Hs_CloudIsFactQuestion(const std::string& text)
{
    static const std::unordered_set<std::string> kOpeners = {
        "what", "whats", "who", "whos", "whose", "where", "wheres", "when", "why", "how", "hows", "which",
        "is", "are", "was", "were", "do", "does", "did", "can", "could", "will", "would", "should",
    };
    static const std::unordered_set<std::string> kSecondPerson = {
        "you", "u", "ur", "your", "youre", "yours", "yourself", "ya", "yall",
    };

    std::vector<std::string> words = Words(text);
    if (words.size() < 2)
        return false;

    if (text.find('?') == std::string::npos && !kOpeners.count(words.front()))
        return false;

    bool secondPerson = false, asksToKnow = false;
    for (std::string const& w : words)
    {
        if (kSecondPerson.count(w))
            secondPerson = true;
        if (w == "know" || w == "remember")
            asksToKnow = true;
    }
    return !secondPerson || asksToKnow;
}

HsCloudReply Hs_ParseCloudReply(const std::string& raw)
{
    HsCloudReply out;

    // One line: collapse every whitespace run, newlines included.
    std::string text;
    bool space = false;
    for (char c : raw)
    {
        if (std::isspace(static_cast<unsigned char>(c)))
            space = true;
        else
        {
            if (space && !text.empty())
                text += ' ';
            space = false;
            text += c;
        }
    }

    // The expiry label, wherever it landed; removed from the fact either way.
    std::string lower = Lower(text);
    size_t tag = lower.find("[days:");
    if (tag != std::string::npos)
    {
        size_t close = lower.find(']', tag);
        if (close != std::string::npos)
        {
            std::string digits;
            for (size_t i = tag + 6; i < close; ++i)
                if (std::isdigit(static_cast<unsigned char>(lower[i])))
                    digits += lower[i];
            if (!digits.empty() && digits.size() <= 5)
                out.days = static_cast<uint32_t>(std::stoul(digits));
            text.erase(tag, close - tag + 1);
            text = Trim(text);
        }
    }

    size_t sep = text.find(" -- ");
    if (sep == std::string::npos)
        return out; // "unknown", or prose that skipped the format: both mean no fact

    std::string title = Trim(text.substr(0, sep));
    size_t colon = title.rfind(':');
    if (colon != std::string::npos)
        title = Trim(title.substr(colon + 1));
    while (!title.empty() && (title.front() == '*' || title.front() == '"'))
        title.erase(0, 1);
    while (!title.empty() && (title.back() == '*' || title.back() == '"'))
        title.pop_back();

    std::string sentence = Trim(text.substr(sep + 4));
    while (!sentence.empty() && (sentence.back() == '*' || sentence.back() == '"'))
        sentence.pop_back();

    // A title is a name, not a paragraph; a sentence is one sentence.
    constexpr size_t kTitleMax = 80, kSentenceMax = 300;
    if (title.empty() || title.size() > kTitleMax || sentence.empty())
        return out;
    // "unknown -- The message is casual chat..." (measured: 58 of 112 refusals
    // came back this way) is a refusal with a reason attached, not a fact.
    if (Lower(title) == "unknown")
        return out;
    if (Lower(sentence) == "unknown" || Lower(sentence) == "unknown.")
        return out;
    if (sentence.size() > kSentenceMax)
    {
        size_t cut = sentence.rfind(". ", kSentenceMax);
        sentence = cut != std::string::npos ? sentence.substr(0, cut + 1) : sentence.substr(0, kSentenceMax);
    }

    out.known = true;
    out.fact  = title + " -- " + sentence;
    return out;
}

std::vector<std::string> Hs_CloudFactTerms(const std::string& text)
{
    static const std::unordered_set<std::string> kDrop = {
        "a", "an", "the", "is", "are", "was", "were", "be", "been", "am", "what", "whats", "who", "whos",
        "whose", "when", "where", "wheres", "why", "how", "hows", "which", "do", "does", "did", "of", "in",
        "on", "at", "to", "for", "and", "or", "it", "its", "that", "this", "by", "with", "from", "as",
        "u", "you", "ya", "know", "tell", "me", "i", "any", "anyone", "can", "could", "would", "will",
        "should", "still", "really", "actually", "even", "just", "so", "there", "theres", "about",
    };
    std::vector<std::string> out;
    for (std::string w : Words(text))
    {
        if (kDrop.count(w))
            continue;
        if (w.size() > 3 && w.back() == 's' && !IsNumber(w))
            w.pop_back();
        if (std::find(out.begin(), out.end(), w) == out.end())
            out.push_back(w);
    }
    return out;
}

bool Hs_CloudMentionsRelativeDay(const std::string& text)
{
    static const std::unordered_set<std::string> kRelative = {
        "today", "tonight", "yesterday", "tomorrow", "tmrw", "tmr", "tonite", "night",
    };
    for (std::string const& w : Words(text))
        if (kRelative.count(w))
            return true;
    return false;
}

namespace
{
    // Dice over two term sets, 0 when the numbers in them differ at all.
    float MatchScore(std::vector<std::string> const& a, std::vector<std::string> const& b)
    {
        if (a.empty() || b.empty())
            return 0.0f;
        size_t shared = 0;
        for (std::string const& t : a)
        {
            bool inB = std::find(b.begin(), b.end(), t) != b.end();
            if (inB)
                ++shared;
            else if (IsNumber(t))
                return 0.0f;
        }
        for (std::string const& t : b)
            if (IsNumber(t) && std::find(a.begin(), a.end(), t) == a.end())
                return 0.0f;
        return 2.0f * static_cast<float>(shared) / static_cast<float>(a.size() + b.size());
    }
}

void HsCloudFactStore::Put(const HsCloudFactEntry& entry)
{
    std::vector<std::string> terms = Hs_CloudFactTerms(entry.question);
    if (terms.empty())
        return;
    std::vector<std::string> sorted = terms;
    std::sort(sorted.begin(), sorted.end());
    for (Stored& s : _entries)
    {
        std::vector<std::string> other = s.terms;
        std::sort(other.begin(), other.end());
        if (other == sorted)
        {
            s.entry = entry;
            s.terms = terms;
            return;
        }
    }
    _entries.push_back({ entry, terms });
}

HsCloudFactEntry const* HsCloudFactStore::Find(const std::string& question, uint64_t now) const
{
    std::vector<std::string> terms = Hs_CloudFactTerms(question);
    HsCloudFactEntry const* best = nullptr;
    float bestScore = kMatchMin;
    for (Stored const& s : _entries)
    {
        if (s.entry.expiresAt <= now)
            continue;
        float score = MatchScore(terms, s.terms);
        if (score >= bestScore)
        {
            bestScore = score;
            best      = &s.entry;
        }
    }
    return best;
}

size_t HsCloudFactStore::PruneExpired(uint64_t now)
{
    size_t before = _entries.size();
    _entries.erase(std::remove_if(_entries.begin(), _entries.end(),
                                  [now](Stored const& s) { return s.entry.expiresAt <= now; }),
                   _entries.end());
    return before - _entries.size();
}

bool HsCloudDailyLimiter::TryTake(uint32_t account, uint32_t day, uint32_t limit)
{
    auto& slot = _counts[account];
    if (slot.first != day)
        slot = { day, 0 };
    if (slot.second >= limit)
        return false;
    ++slot.second;

    // Yesterday's accounts are dead weight; drop them once the map has grown.
    if (_counts.size() > 4096)
        for (auto it = _counts.begin(); it != _counts.end(); )
            it = it->second.first != day ? _counts.erase(it) : std::next(it);
    return true;
}

uint32_t HsCloudDailyLimiter::Used(uint32_t account, uint32_t day) const
{
    auto it = _counts.find(account);
    return (it != _counts.end() && it->second.first == day) ? it->second.second : 0;
}
