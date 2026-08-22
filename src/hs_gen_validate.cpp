#include "hs_gen_validate.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <unordered_set>

namespace
{
    // §4.7's two placeholder classes. Universal ones resolve for all 5000
    // bots straight off the character row; card-only ones resolve only for
    // carded bots (step 15) and would fail silently for everyone else, so a
    // non-card-gated category must never accept them.
    const std::unordered_set<std::string> kUniversalPlaceholders = {
        "%item_link", "%quest_link", "%class", "%level", "%zone", "%guild",
    };
    const std::unordered_set<std::string> kCardOnlyPlaceholders = {
        "%main_focus", "%current_goal",
    };

    // Deliberately short and low-collision: WoW chat legitimately uses
    // words like "cap" (level cap) and "based" would be too noisy to trust
    // as a single token, so the list sticks to slang with little chance of
    // colliding with ordinary WoW-flavor prose. Not exhaustive by design
    // (§4.7 names this a cheap regex-shaped gate, not a rule engine).
    const std::vector<std::string> kSlangTokens = {
        "lol", "lmao", "rofl", "bruh", "bestie", "ngl", "sus", "bussin", "yeet", "rn",
    };

    const std::vector<std::string> kQuestionLeadWords = {
        "what", "who", "when", "where", "why", "how", "are", "do", "does", "did", "is", "anyone", "can",
    };

    std::string ToLower(const std::string& s)
    {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    std::string Trim(const std::string& s)
    {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    // Splits on anything that isn't a letter, digit, or the '%' that starts
    // a placeholder token, so "%item_link" tokenizes as one token and
    // ordinary punctuation never survives into the token set.
    std::vector<std::string> Tokenize(const std::string& text)
    {
        std::string lower = ToLower(text);
        std::vector<std::string> tokens;
        std::string current;
        for (size_t i = 0; i < lower.size(); ++i)
        {
            char c = lower[i];
            bool isWordChar = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || (c == '%' && current.empty());
            if (isWordChar)
            {
                current += c;
            }
            else
            {
                if (!current.empty()) { tokens.push_back(current); current.clear(); }
            }
        }
        if (!current.empty())
            tokens.push_back(current);
        return tokens;
    }

    // Extracts every %word token, independent of whether it's recognized.
    std::vector<std::string> ExtractPlaceholders(const std::string& text)
    {
        std::vector<std::string> found;
        for (auto const& tok : Tokenize(text))
            if (!tok.empty() && tok[0] == '%' && tok.size() > 1)
                found.push_back(tok);
        return found;
    }

    // Same codepoint-range scan hs_style.cpp's StripEmoji uses to decide
    // what to drop -- here just a detector, not a stripper.
    bool ScanForEmoji(const std::string& in)
    {
        size_t i = 0;
        while (i < in.size())
        {
            unsigned char c = static_cast<unsigned char>(in[i]);
            uint32_t codepoint = 0;
            size_t   len       = 1;

            if      (c < 0x80)                                         { ++i; continue; }
            else if ((c & 0xE0) == 0xC0 && i + 1 < in.size())          { codepoint = c & 0x1F; len = 2; }
            else if ((c & 0xF0) == 0xE0 && i + 2 < in.size())          { codepoint = c & 0x0F; len = 3; }
            else if ((c & 0xF8) == 0xF0 && i + 3 < in.size())          { codepoint = c & 0x07; len = 4; }
            else { ++i; continue; }

            bool validContinuation = true;
            for (size_t k = 1; k < len; ++k)
            {
                unsigned char cc = static_cast<unsigned char>(in[i + k]);
                if ((cc & 0xC0) != 0x80) { validContinuation = false; break; }
                codepoint = (codepoint << 6) | (cc & 0x3F);
            }
            if (!validContinuation) { ++i; continue; }

            bool isEmoji =
                (codepoint >= 0x1F300 && codepoint <= 0x1FAFF) ||
                (codepoint >= 0x2600  && codepoint <= 0x27BF)  ||
                codepoint == 0x2B50 || codepoint == 0x2764 ||
                codepoint == 0xFE0F || codepoint == 0x200D;
            if (isEmoji)
                return true;

            i += len;
        }
        return false;
    }

    // §4.4's named corruption marker -- the utf8mb3-era mojibake problem
    // that motivated authoring the corpus fresh rather than importing stock
    // rows. U+FFFD REPLACEMENT CHARACTER, encoded as EF BF BD in UTF-8.
    bool ContainsReplacementChar(const std::string& s)
    {
        return s.find("\xEF\xBF\xBD") != std::string::npos;
    }

    bool ContainsMarkdownOrQuoteChars(const std::string& s)
    {
        // No underscore: legitimate placeholder tokens (%item_link etc) use
        // one, so it can't be a blanket reject or every placeholder-bearing
        // category would fail here before the placeholder check ever runs.
        static const std::string kBadChars = "\"`*[]{}";
        return s.find_first_of(kBadChars) != std::string::npos || ContainsReplacementChar(s);
    }

    bool ReadsAsQuestion(const std::string& trimmed)
    {
        if (!trimmed.empty() && trimmed.back() == '?')
            return true;
        std::vector<std::string> tokens = Tokenize(trimmed);
        if (tokens.empty())
            return false;
        return std::find(kQuestionLeadWords.begin(), kQuestionLeadWords.end(), tokens.front()) != kQuestionLeadWords.end();
    }

    bool ContainsSlang(const std::string& trimmed)
    {
        std::vector<std::string> tokens = Tokenize(trimmed);
        for (auto const& tok : tokens)
            if (std::find(kSlangTokens.begin(), kSlangTokens.end(), tok) != kSlangTokens.end())
                return true;
        return false;
    }
}

bool Hs_ContainsPlaceholder(const std::string& text)
{
    return !ExtractPlaceholders(text).empty();
}

double Hs_JaccardSimilarity(const std::string& a, const std::string& b)
{
    std::vector<std::string> tokensA = Tokenize(a);
    std::vector<std::string> tokensB = Tokenize(b);
    std::set<std::string> setA(tokensA.begin(), tokensA.end());
    std::set<std::string> setB(tokensB.begin(), tokensB.end());
    if (setA.empty() && setB.empty())
        return 1.0;

    size_t intersection = 0;
    for (auto const& tok : setA)
        if (setB.count(tok))
            ++intersection;

    size_t unionSize = setA.size() + setB.size() - intersection;
    return unionSize == 0 ? 0.0 : static_cast<double>(intersection) / static_cast<double>(unionSize);
}

HsGenVerdict Hs_QualityGate(const std::string& candidate, bool allowQuestions)
{
    std::string trimmed = Trim(candidate);

    if (trimmed.size() < 10)
        return { false, "too_short" };
    if (trimmed.size() > 180)
        return { false, "too_long" };
    if (ContainsMarkdownOrQuoteChars(trimmed))
        return { false, "markdown_or_quote_chars" };
    if (ScanForEmoji(trimmed))
        return { false, "emoji" };
    if (ContainsSlang(trimmed))
        return { false, "modern_slang" };
    if (!allowQuestions && ReadsAsQuestion(trimmed))
        return { false, "reads_as_question" };

    return { true, "" };
}

HsGenVerdict Hs_PlaceholderDiscipline(const std::string& candidate,
                                       const std::vector<std::string>& existingRows,
                                       bool categoryCardGated)
{
    bool exemplarsUsePlaceholder = false;
    for (auto const& row : existingRows)
        if (Hs_ContainsPlaceholder(row)) { exemplarsUsePlaceholder = true; break; }

    std::vector<std::string> candidatePlaceholders = ExtractPlaceholders(candidate);

    if (exemplarsUsePlaceholder && candidatePlaceholders.empty())
        return { false, "missing_required_placeholder" };

    for (auto const& ph : candidatePlaceholders)
    {
        bool known = kUniversalPlaceholders.count(ph) || (categoryCardGated && kCardOnlyPlaceholders.count(ph));
        if (!known)
            return { false, "unknown_placeholder" };
    }

    return { true, "" };
}

HsGenVerdict Hs_DedupCheck(const std::string& candidate, const std::vector<std::string>& existingRows)
{
    constexpr double kDedupThreshold = 0.6; // §4.7: "reject above ~0.6"
    for (auto const& row : existingRows)
        if (Hs_JaccardSimilarity(candidate, row) > kDedupThreshold)
            return { false, "too_similar_to_existing" };
    return { true, "" };
}

HsGenVerdict Hs_EvaluateCandidate(const std::string& candidate,
                                   const std::vector<std::string>& existingRows,
                                   bool categoryCardGated)
{
    HsGenVerdict quality = Hs_QualityGate(candidate);
    if (!quality.accepted)
        return quality;

    HsGenVerdict placeholder = Hs_PlaceholderDiscipline(candidate, existingRows, categoryCardGated);
    if (!placeholder.accepted)
        return placeholder;

    return Hs_DedupCheck(candidate, existingRows);
}
