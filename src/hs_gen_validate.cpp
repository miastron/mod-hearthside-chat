#include "hs_gen_validate.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <unordered_set>

namespace
{
    // Two placeholder classes. Universal ones resolve for all 5000 bots
    // straight off the character row; card-only ones resolve only for
    // carded bots and would fail silently for everyone else, so a
    // non-card-gated category must never accept them.
    const std::unordered_set<std::string> kUniversalPlaceholders = {
        "%item_link", "%quest_link", "%class", "%level", "%zone", "%guild",
    };
    const std::unordered_set<std::string> kCardOnlyPlaceholders = {
        "%main_focus", "%current_goal",
    };

    // Scripted bot-to-bot dialogue's own token vocabulary: disjoint from
    // the corpus sets above, resolved per cast pair rather than per bot
    // (see hs_corpus.h's Hs_ResolveScriptPlaceholders).
    const std::unordered_set<std::string> kScriptPlaceholders = {
        "%my_class", "%my_level", "%my_zone", "%my_guild",
        "%other_class", "%other_level", "%other_zone", "%other_guild",
    };

    // Deliberately short and low-collision: WoW chat legitimately uses
    // words like "cap" (level cap), and "based" would be too noisy to
    // trust as a single token, so the list sticks to slang with little
    // chance of colliding with ordinary WoW-flavor prose. Not exhaustive
    // by design: this is a cheap regex-shaped gate, not a rule engine.
    const std::vector<std::string> kSlangTokens = {
        "lol", "lmao", "rofl", "bruh", "bestie", "ngl", "sus", "bussin", "yeet", "rn",
    };

    const std::vector<std::string> kQuestionLeadWords = {
        "what", "who", "when", "where", "why", "how", "are", "do", "does", "did", "is", "anyone", "can",
    };

    // Words that only make sense as the first word of a *reply*. A corpus
    // line is spoken unprompted, so opening with agreement, a conjunction,
    // or a greeting means the line is answering something that was never
    // said. See ReadsAsReply below for the evidence this list came from.
    const std::vector<std::string> kReplyLeadWords = {
        // agreement / acknowledgement
        "yeah", "yea", "yep", "yup", "nah", "nope", "ok", "okay", "sure", "right",
        "exactly", "agreed", "true", "same", "thanks", "glad", "guess", "honestly",
        // conjunctions continuing someone else's sentence
        "and", "but", "so", "or", "also", "plus", "anyway", "besides",
        // interjections that answer a turn
        "oh", "ah", "huh", "well",
        // greetings: addressed at a person, not the channel
        "hello", "hi", "hey", "sup", "welcome",
    };

    // Demonstratives pointing at something the line never names. Leading
    // "it"/"this" are deliberately absent: "it's always the last boss" is
    // ordinary standalone commentary, while "that was rough" is not.
    //
    // Only counts when the demonstrative is *bare* -- followed by a verb or
    // by "one", not by the noun it is pointing at. "these quest chains are
    // getting long" names its own subject and is fine; "that was rough" and
    // "that one still hurts" do not.
    const std::vector<std::string> kAnaphoricLeadWords = {
        "that", "those", "these", "them",
    };
    const std::vector<std::string> kBareDemonstrativeFollowers = {
        "is", "are", "was", "were", "s", "re", "ll", "d", "one", "ones", "aint",
    };

    // The same words as the *last* word of a line, where they can only be
    // pointing back at a turn that isn't there ("i'll probably sell it",
    // "i meant that").
    //
    // Only applied to a short line. Past about six words a line usually
    // names its own antecedent before the pronoun -- "people only notice my
    // aura when i forget to switch it", "the troggs pushing down on
    // thelsamar respawn faster than i can clear them" -- and the seed-corpus
    // harness caught ten of those before this bound went in.
    const std::vector<std::string> kAnaphoricTailWords = {
        "that", "it", "this", "one", "them", "those", "him", "her", "then",
    };
    constexpr size_t kMaxTokensForTailAnaphora = 6;

    // A stored line is replayed for any player at any time, so it can never
    // say how things are *now* relative to before. BuildGenerationPrompt
    // (hs_generator.cpp) has told the model this since the beginning; the
    // gate never enforced it.
    //
    // "still" is deliberately absent, despite being on that prompt's own
    // list. It looked like the clearest case in
    // BuildGenerationPrompt's list, but the hand-authored corpus uses it for
    // statements that are timeless rather than comparative -- "ironforge's
    // still my favorite city", "icecrown still gives me a chill every time",
    // "still figuring out where everything is around here" -- and the
    // seed-corpus harness flagged nine of them at once. The word does not
    // carry the claim; the comparison does.
    const std::vector<std::string> kTrendWords = {
        "lately", "nowadays", "recently", "anymore",
    };
    const std::vector<std::string> kTrendPhrases = {
        "these days", "more than usual", "used to be", "than it used to",
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
    // what to drop: here just a detector, not a stripper.
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

    // The utf8mb3-era mojibake problem that motivated authoring the corpus
    // fresh rather than importing stock rows. U+FFFD REPLACEMENT CHARACTER,
    // encoded as EF BF BD in UTF-8.
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

    // Does this read as a turn in a conversation rather than a line
    // somebody typed into an empty channel?
    //
    // Added 2026-09-13 off live-realm evidence. The generator had written
    // 1032 of the 1238 rows in hside_corpus, and a large share of them were
    // reply fragments with nothing to reply to: "guess that's on me", "i'm
    // doing alright, thanks for asking", "and another one, for anyone else
    // reading this", "same as before", "i'll be here until then". Delivered
    // unprompted into /say or General, every one of them reads as somebody
    // answering a question nobody asked -- which is a more obvious tell than
    // a dull line, because a real player's chat is never shaped like that.
    //
    // BuildGenerationPrompt already asks for "not addressed to anyone, first
    // person" and bans trend words. Nothing enforced either, and a 1B model
    // does not reliably honour a paragraph of guidance it was never trained
    // on. So the rule moves here, where it is deterministic and testable.
    //
    // Precision over recall on purpose: a rejected candidate costs one more
    // generation cycle on an idle-time task, while an accepted bad row is
    // replayed to players until something evicts it.
    bool ReadsAsReply(const std::string& trimmed)
    {
        std::vector<std::string> tokens = Tokenize(ToLower(trimmed));
        if (tokens.empty())
            return false;

        if (std::find(kReplyLeadWords.begin(), kReplyLeadWords.end(), tokens.front()) != kReplyLeadWords.end())
            return true;
        if (std::find(kAnaphoricLeadWords.begin(), kAnaphoricLeadWords.end(), tokens.front()) != kAnaphoricLeadWords.end()
            && (tokens.size() == 1
                || std::find(kBareDemonstrativeFollowers.begin(), kBareDemonstrativeFollowers.end(), tokens[1])
                       != kBareDemonstrativeFollowers.end()))
            return true;
        if (tokens.size() <= kMaxTokensForTailAnaphora
            && std::find(kAnaphoricTailWords.begin(), kAnaphoricTailWords.end(), tokens.back()) != kAnaphoricTailWords.end())
            return true;

        // "that one"/"this one" anywhere: the line is singling out a thing
        // the listener is expected to already have in mind.
        for (size_t i = 0; i + 1 < tokens.size(); ++i)
            if ((tokens[i] == "that" || tokens[i] == "this") && tokens[i + 1] == "one")
                return true;

        return false;
    }

    bool ReferencesTrend(const std::string& trimmed)
    {
        std::string lowered = ToLower(trimmed);
        for (auto const& phrase : kTrendPhrases)
            if (lowered.find(phrase) != std::string::npos)
                return true;

        std::vector<std::string> tokens = Tokenize(lowered);
        for (auto const& tok : tokens)
            if (std::find(kTrendWords.begin(), kTrendWords.end(), tok) != kTrendWords.end())
                return true;
        return false;
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

bool Hs_CategoryIsResponse(const std::string& category)
{
    // Prefix match rather than a list: every opener_* bucket is reactive by
    // construction (hs_opener.h), and a new one should inherit that without
    // needing this file touched.
    return category.rfind("opener_", 0) == 0;
}

HsGenVerdict Hs_QualityGate(const std::string& candidate, bool allowQuestions, bool allowShort,
                             bool allowReply)
{
    std::string trimmed = Trim(candidate);

    if (trimmed.empty())
        return { false, "too_short" };
    if (!allowShort && trimmed.size() < 10)
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

    // Both gated on !allowShort, which is the module's existing "this line
    // has to stand alone with no surrounding context" predicate (see the
    // header). A scripted bot-to-bot turn sits in a back-and-forth, so
    // answering the previous turn is exactly what it should do, and a
    // script is spoken once rather than stored and replayed, so a trend
    // word in one is not the lie it would be in a corpus row.
    bool mustStandAlone = !allowShort && !allowReply;
    if (mustStandAlone && ReadsAsReply(trimmed))
        return { false, "reads_as_reply" };
    if (mustStandAlone && ReferencesTrend(trimmed))
        return { false, "references_trend" };

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
    constexpr double kDedupThreshold = 0.6; // reject above ~0.6
    for (auto const& row : existingRows)
        if (Hs_JaccardSimilarity(candidate, row) > kDedupThreshold)
            return { false, "too_similar_to_existing" };
    return { true, "" };
}

HsGenVerdict Hs_ScriptPlaceholderDiscipline(const std::string& candidate)
{
    for (auto const& ph : ExtractPlaceholders(candidate))
        if (!kScriptPlaceholders.count(ph))
            return { false, "unknown_script_placeholder" };
    return { true, "" };
}

HsGenVerdict Hs_EvaluateCandidate(const std::string& candidate,
                                   const std::vector<std::string>& existingRows,
                                   bool categoryCardGated,
                                   bool categoryIsResponse)
{
    HsGenVerdict quality = Hs_QualityGate(candidate, /*allowQuestions=*/false, /*allowShort=*/false,
                                           /*allowReply=*/categoryIsResponse);
    if (!quality.accepted)
        return quality;

    HsGenVerdict placeholder = Hs_PlaceholderDiscipline(candidate, existingRows, categoryCardGated);
    if (!placeholder.accepted)
        return placeholder;

    return Hs_DedupCheck(candidate, existingRows);
}
