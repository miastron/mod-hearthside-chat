#include "hs_style.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // The care table collapses to four bands; every transform below
    // switches on this rather than re-deriving thresholds from a raw float.
    enum class StyleBand
    {
        Sloppy,  // 0.0-0.3
        Loose,   // 0.3-0.6
        Careful, // 0.6-0.8
        Precise, // 0.8-1.0
    };

    StyleBand BandForCare(float care)
    {
        if (care < 0.3f) return StyleBand::Sloppy;
        if (care < 0.6f) return StyleBand::Loose;
        if (care < 0.8f) return StyleBand::Careful;
        return StyleBand::Precise;
    }

    std::string ToLowerAscii(const std::string& s)
    {
        std::string out = s;
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    // Shared helper -- used by both the casing section below and
    // StripRestatingLeadIn (LLM-tell stripping).
    std::string CapitalizeFirstAlpha(std::string s)
    {
        for (char& c : s)
        {
            if (std::isalpha(static_cast<unsigned char>(c)))
            {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                break;
            }
        }
        return s;
    }

    // Placeholder marker (SOH, never occurs in real chat text) used to mask
    // protected spans (item links, hyperlinks, slash commands) out of every
    // transform below.
    constexpr char kPlaceholderMark = '\x01';

    const std::unordered_set<std::string>& ProtectedWords()
    {
        // The tier-0 reflex vocabulary, plus tokens the abbreviation
        // transform below produces (e.g. "ty", "w/") -- already the
        // compressed form, so typo'ing them would destroy their function.
        // Checked here because they must survive the typo pass that runs
        // immediately after abbreviation.
        static const std::unordered_set<std::string> words = {
            "gz", "ty", "inv", "sum", "wtb", "wts", "lol", "wb", "rdy", "w/", "brb",
        };
        return words;
    }

    // ---- protected-span extraction (item links, hyperlinks, slash commands) ----

    // Full WoW chat hyperlink markup (|cAARRGGBB|Hitem:...|h[Name]|h|r) must
    // be masked as one atomic unit -- corrupting the |H control sequence
    // breaks a clickable link, not just the visible text. Bare [Name]-only
    // brackets and /commands get the same treatment.
    std::string ExtractProtectedSpans(const std::string& text, std::vector<std::string>& spans)
    {
        static const std::regex kHyperlink(R"(\|c[0-9A-Fa-f]{8}\|H[^|]*\|h\[[^\]]*\]\|h\|r)");
        static const std::regex kBareLink(R"(\[[^\]]*\])");
        static const std::regex kSlashCmd(R"(/[A-Za-z][A-Za-z0-9_]*)");

        std::string working = text;
        for (const std::regex* pattern : { &kHyperlink, &kBareLink, &kSlashCmd })
        {
            std::string next;
            next.reserve(working.size());
            size_t lastPos = 0;
            for (auto it = std::sregex_iterator(working.begin(), working.end(), *pattern);
                 it != std::sregex_iterator(); ++it)
            {
                auto match = *it;
                size_t pos = static_cast<size_t>(match.position());
                next.append(working, lastPos, pos - lastPos);
                next += kPlaceholderMark;
                next += std::to_string(spans.size());
                next += kPlaceholderMark;
                spans.push_back(match.str());
                lastPos = pos + static_cast<size_t>(match.length());
            }
            next.append(working, lastPos, working.size() - lastPos);
            working = next;
        }
        return working;
    }

    // Masks every case-insensitive occurrence of `phrase` as one atomic
    // span, same placeholder-marker scheme as ExtractProtectedSpans -- so a
    // multi-word verbal tic ("no worries") is protected as a unit rather
    // than needing per-word matching the way ProtectedWords() does for
    // single tokens. No-op if `phrase` is empty (the uncarded/no-tic case).
    std::string MaskLiteralPhrase(const std::string& text, const std::string& phrase, std::vector<std::string>& spans)
    {
        if (phrase.empty())
            return text;

        std::string lowerText   = ToLowerAscii(text);
        std::string lowerPhrase = ToLowerAscii(phrase);

        std::string out;
        out.reserve(text.size());
        size_t i = 0;
        while (i < text.size())
        {
            if (i + lowerPhrase.size() <= lowerText.size() &&
                lowerText.compare(i, lowerPhrase.size(), lowerPhrase) == 0)
            {
                out += kPlaceholderMark;
                out += std::to_string(spans.size());
                out += kPlaceholderMark;
                spans.push_back(text.substr(i, phrase.size()));
                i += phrase.size();
            }
            else
            {
                out += text[i];
                ++i;
            }
        }
        return out;
    }

    std::string RestoreProtectedSpans(const std::string& text, const std::vector<std::string>& spans)
    {
        std::string out = text;
        for (size_t i = 0; i < spans.size(); ++i)
        {
            std::string token = std::string(1, kPlaceholderMark) + std::to_string(i) + kPlaceholderMark;
            size_t pos = out.find(token);
            if (pos != std::string::npos)
                out.replace(pos, token.size(), spans[i]);
        }
        return out;
    }

    // ---- LLM-tell stripping ----

    // UTF-8 decode/reencode dropping codepoints in the common emoji blocks.
    // Malformed sequences are passed through unchanged rather than rejected --
    // this is a cheap cosmetic filter, not a validator.
    std::string StripEmoji(const std::string& in)
    {
        std::string out;
        out.reserve(in.size());
        size_t i = 0;
        while (i < in.size())
        {
            unsigned char c = static_cast<unsigned char>(in[i]);
            uint32_t codepoint = 0;
            size_t len = 1;

            if ((c & 0x80) == 0x00)                                    { codepoint = c;        len = 1; }
            else if ((c & 0xE0) == 0xC0 && i + 1 < in.size())          { codepoint = c & 0x1F; len = 2; }
            else if ((c & 0xF0) == 0xE0 && i + 2 < in.size())          { codepoint = c & 0x0F; len = 3; }
            else if ((c & 0xF8) == 0xF0 && i + 3 < in.size())          { codepoint = c & 0x07; len = 4; }
            else { out += static_cast<char>(c); ++i; continue; }

            bool validContinuation = true;
            for (size_t k = 1; k < len; ++k)
            {
                unsigned char cc = static_cast<unsigned char>(in[i + k]);
                if ((cc & 0xC0) != 0x80) { validContinuation = false; break; }
                codepoint = (codepoint << 6) | (cc & 0x3F);
            }
            if (!validContinuation) { out += static_cast<char>(c); ++i; continue; }

            bool isEmoji =
                (codepoint >= 0x1F300 && codepoint <= 0x1FAFF) ||
                (codepoint >= 0x2600  && codepoint <= 0x27BF)  ||
                codepoint == 0x2B50 || codepoint == 0x2764 ||
                codepoint == 0xFE0F || codepoint == 0x200D;

            if (!isEmoji)
                out.append(in, i, len);
            i += len;
        }
        return out;
    }

    std::string CollapseWhitespace(const std::string& in)
    {
        std::string out;
        out.reserve(in.size());
        bool lastWasSpace = false;
        for (char c : in)
        {
            bool isSpace = (c == ' ' || c == '\t');
            if (isSpace && lastWasSpace)
                continue;
            out += c;
            lastWasSpace = isSpace;
        }
        size_t start = out.find_first_not_of(' ');
        size_t end   = out.find_last_not_of(' ');
        if (start == std::string::npos)
            return "";
        return out.substr(start, end - start + 1);
    }

    // Replaces every occurrence of `dash`, absorbing any spaces immediately
    // around it, with a single ", " — so "rough -- you'll" becomes
    // "rough, you'll" rather than "rough ,  you'll".
    std::string StripDashes(std::string s, const std::string& dash)
    {
        size_t pos = 0;
        while ((pos = s.find(dash, pos)) != std::string::npos)
        {
            size_t start = pos;
            while (start > 0 && s[start - 1] == ' ')
                --start;
            size_t end = pos + dash.size();
            while (end < s.size() && s[end] == ' ')
                ++end;
            s.replace(start, end - start, ", ");
            pos = start + 2;
        }
        return s;
    }

    // Detects "restating the question" as an LLM tell. A semantic check
    // against the trigger text would be unsafe -- a bot directly answering a
    // factual question ("where's the AH?" -> "the AH is in Orgrimmar")
    // legitimately shares nouns with the question, and a word-overlap
    // heuristic would gut real answers along with the tell. Instead this
    // matches a short list of explicit meta-referential openers that are
    // themselves the tell no matter what follows, so no trigger text is
    // needed at all.
    std::string StripRestatingLeadIn(const std::string& text)
    {
        static const std::vector<std::string> kLeadIns = {
            "so you're asking", "so you are asking", "so you want to know",
            "you're asking", "you are asking", "you want to know",
            "to answer your question", "in answer to your question",
            "you're wondering", "you are wondering", "so you were asking",
        };

        std::string lower = ToLowerAscii(text);
        for (const std::string& leadIn : kLeadIns)
        {
            if (lower.rfind(leadIn, 0) != 0)
                continue;

            // The restated question ends, and the real answer begins, at
            // the next clause boundary. If none turns up nearby this
            // probably isn't the tell after all -- leave the text alone
            // rather than guess where to cut.
            size_t searchFrom = leadIn.size();
            size_t searchLimit = std::min(text.size(), searchFrom + 100);
            size_t boundary = std::string::npos;
            for (char punct : { ',', '?', '!', '.' })
            {
                size_t pos = text.find(punct, searchFrom);
                if (pos != std::string::npos && pos < searchLimit && (boundary == std::string::npos || pos < boundary))
                    boundary = pos;
            }
            if (boundary == std::string::npos)
                continue;

            std::string rest = text.substr(boundary + 1);
            size_t firstNonSpace = rest.find_first_not_of(' ');
            rest = (firstNonSpace == std::string::npos) ? "" : rest.substr(firstNonSpace);
            return CapitalizeFirstAlpha(rest);
        }
        return text;
    }

    // Strips known LLM tells: em dash, leading "Ah,", emoji, restating the
    // question. All four are literal and mechanical.
    std::string StripLLMTells(const std::string& text)
    {
        std::string s = StripEmoji(text);
        s = StripDashes(s, "\xE2\x80\x94"); // em dash, U+2014
        s = StripDashes(s, "--");

        std::string lower = ToLowerAscii(s);
        if (lower.rfind("ah, ", 0) == 0)
            s.erase(0, 4);
        else if (lower.rfind("ah,", 0) == 0)
            s.erase(0, 3);

        s = StripRestatingLeadIn(s);

        return CollapseWhitespace(s);
    }

    // ---- word helpers ----

    std::vector<std::string> SplitWords(const std::string& s)
    {
        std::vector<std::string> words;
        std::istringstream iss(s);
        std::string w;
        while (iss >> w)
            words.push_back(w);
        return words;
    }

    std::string JoinWords(const std::vector<std::string>& words)
    {
        std::string out;
        for (size_t i = 0; i < words.size(); ++i)
        {
            if (i) out += ' ';
            out += words[i];
        }
        return out;
    }

    std::string SplitTrailingPunct(const std::string& word, std::string& trailingPunctOut)
    {
        static const std::string kPunct = ".,!?;:";
        size_t end = word.size();
        while (end > 0 && kPunct.find(word[end - 1]) != std::string::npos)
            --end;
        trailingPunctOut = word.substr(end);
        return word.substr(0, end);
    }

    bool ContainsDigit(const std::string& s)
    {
        return std::any_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
    }

    bool IsPlaceholderToken(const std::string& word)
    {
        return word.find(kPlaceholderMark) != std::string::npos;
    }

    // Protected-token list: item links / slash commands (masked out before
    // this ever runs), digits and money strings, the reflex/abbreviation
    // vocabulary, and the bot's or sender's own name.
    bool IsProtectedToken(const std::string& word, const std::string& botName, const std::string& senderName)
    {
        if (IsPlaceholderToken(word))
            return true;
        if (ContainsDigit(word))
            return true;

        std::string trailingPunct;
        std::string lower = ToLowerAscii(SplitTrailingPunct(word, trailingPunct));

        if (ProtectedWords().count(lower))
            return true;
        if (!botName.empty() && lower == ToLowerAscii(botName))
            return true;
        if (!senderName.empty() && lower == ToLowerAscii(senderName))
            return true;

        return false;
    }

    // ---- casing ----

    // care table: 0.0-0.3 none, 0.3-0.6 none/first-word only, 0.6-0.8+ leave
    // the model's own sentence case alone.
    std::string ApplyCasing(const std::string& text, StyleBand band, std::mt19937& rng)
    {
        if (band == StyleBand::Careful || band == StyleBand::Precise)
            return text;

        std::string lowered = ToLowerAscii(text);
        if (band == StyleBand::Sloppy)
            return lowered;

        std::uniform_real_distribution<float> coin(0.0f, 1.0f);
        return (coin(rng) < 0.5f) ? lowered : CapitalizeFirstAlpha(lowered);
    }

    // care table: none / rare (25%) / usual (75%) / always. Keeps the
    // model's own mark (?, !, ...) when kept; adds "." only if it had none.
    std::string ApplyTerminalPunctuation(const std::string& text, StyleBand band, std::mt19937& rng)
    {
        std::string s = text;
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        if (s.empty())
            return s;

        std::string mark;
        while (!s.empty() && (s.back() == '.' || s.back() == '!' || s.back() == '?'))
        {
            mark.insert(mark.begin(), s.back());
            s.pop_back();
        }

        float keepProbability = 0.0f;
        switch (band)
        {
            case StyleBand::Sloppy:  keepProbability = 0.0f;  break;
            case StyleBand::Loose:   keepProbability = 0.25f; break;
            case StyleBand::Careful: keepProbability = 0.75f; break;
            case StyleBand::Precise: keepProbability = 1.0f;  break;
        }

        std::uniform_real_distribution<float> coin(0.0f, 1.0f);
        if (coin(rng) >= keepProbability)
            return s;
        return s + (mark.empty() ? "." : mark);
    }

    // care table abbreviation levels (heavy/moderate/light/minimal) as a
    // per-matching-word substitution chance. Small curated first-pass
    // dictionary -- extend it as real replies surface more candidates.
    // `abbrevOverrideChance >= 0` bypasses the band entirely: some
    // archetypes (e.g. TRADER) write heavy abbreviation (WTS, pst)
    // regardless of their care band.
    std::string ApplyAbbreviation(const std::string& text, StyleBand band, float abbrevOverrideChance, std::mt19937& rng)
    {
        float chance;
        if (abbrevOverrideChance >= 0.0f)
        {
            chance = abbrevOverrideChance;
        }
        else
        {
            chance = 0.0f;
            switch (band)
            {
                case StyleBand::Sloppy:  chance = 0.70f; break;
                case StyleBand::Loose:   chance = 0.40f; break;
                case StyleBand::Careful: chance = 0.15f; break;
                case StyleBand::Precise: chance = 0.0f;  break;
            }
        }
        if (chance <= 0.0f)
            return text;

        static const std::unordered_map<std::string, std::string> kAbbrev = {
            { "you", "u" }, { "your", "ur" }, { "you're", "ur" }, { "are", "r" },
            { "for", "4" }, { "to", "2" }, { "too", "2" }, { "thanks", "ty" },
            { "thank", "ty" }, { "with", "w/" }, { "ready", "rdy" }, { "please", "plz" },
            { "okay", "k" }, { "ok", "k" }, { "because", "bc" }, { "before", "b4" },
            { "though", "tho" }, { "probably", "prob" },
        };

        std::vector<std::string> words = SplitWords(text);
        std::uniform_real_distribution<float> coin(0.0f, 1.0f);
        for (auto& word : words)
        {
            if (IsPlaceholderToken(word))
                continue;

            std::string trailingPunct;
            std::string core = SplitTrailingPunct(word, trailingPunct);
            auto it = kAbbrev.find(ToLowerAscii(core));
            if (it == kAbbrev.end())
                continue;
            if (coin(rng) >= chance)
                continue;

            word = it->second + trailingPunct;
        }
        return JoinWords(words);
    }

    // ---- typo injection: six mechanisms ----

    enum class TypoMechanism { MissingApostrophe, Transposition, DroppedLetter, AdjacentKey, DoubledLetter, Homophone };

    const std::unordered_map<std::string, std::string>& HomophoneTable()
    {
        static const std::unordered_map<std::string, std::string> table = {
            { "your", "you're" }, { "you're", "your" }, { "there", "their" }, { "their", "there" },
            { "they're", "their" }, { "its", "it's" }, { "it's", "its" }, { "to", "too" },
            { "too", "to" }, { "then", "than" }, { "than", "then" }, { "loose", "lose" },
            { "lose", "loose" }, { "were", "where" }, { "where", "were" },
        };
        return table;
    }

    const std::unordered_map<char, std::string>& QwertyNeighbors()
    {
        // Same-row horizontal neighbours only — cheap and defensible, not a
        // full keyboard-distance model.
        static const std::unordered_map<char, std::string> table = {
            { 'q', "w" }, { 'w', "qe" }, { 'e', "wr" }, { 'r', "et" }, { 't', "ry" },
            { 'y', "tu" }, { 'u', "yi" }, { 'i', "uo" }, { 'o', "ip" }, { 'p', "o" },
            { 'a', "s" }, { 's', "ad" }, { 'd', "sf" }, { 'f', "dg" }, { 'g', "fh" },
            { 'h', "gj" }, { 'j', "hk" }, { 'k', "jl" }, { 'l', "k" },
            { 'z', "x" }, { 'x', "zc" }, { 'c', "xv" }, { 'v', "cb" }, { 'b', "vn" },
            { 'n', "bm" }, { 'm', "n" },
        };
        return table;
    }

    // word already has trailing punctuation stripped by the caller.
    std::string ApplyOneTypoMechanism(const std::string& word, std::mt19937& rng)
    {
        std::string lower = ToLowerAscii(word);
        bool hasApostrophe = word.find('\'') != std::string::npos;
        bool hasHomophone  = HomophoneTable().count(lower) > 0;

        struct Candidate { TypoMechanism mech; int weight; };
        std::vector<Candidate> candidates;
        if (hasApostrophe)
            candidates.push_back({ TypoMechanism::MissingApostrophe, 30 });
        if (word.size() >= 3)
        {
            candidates.push_back({ TypoMechanism::Transposition, 25 });
            candidates.push_back({ TypoMechanism::DroppedLetter, 15 });
        }
        candidates.push_back({ TypoMechanism::AdjacentKey, 15 });
        candidates.push_back({ TypoMechanism::DoubledLetter, 8 });
        if (hasHomophone)
            candidates.push_back({ TypoMechanism::Homophone, 7 });

        int total = 0;
        for (auto const& c : candidates)
            total += c.weight;
        if (total <= 0)
            return word;

        std::uniform_int_distribution<int> pickDist(0, total - 1);
        int roll = pickDist(rng);
        TypoMechanism chosen = candidates.front().mech;
        for (auto const& c : candidates)
        {
            if (roll < c.weight) { chosen = c.mech; break; }
            roll -= c.weight;
        }

        switch (chosen)
        {
            case TypoMechanism::MissingApostrophe:
            {
                std::string out = word;
                out.erase(std::remove(out.begin(), out.end(), '\''), out.end());
                return out;
            }
            case TypoMechanism::Homophone:
                return HomophoneTable().at(lower);
            case TypoMechanism::Transposition:
            {
                std::uniform_int_distribution<size_t> pos(0, word.size() - 2);
                std::string out = word;
                size_t i = pos(rng);
                std::swap(out[i], out[i + 1]);
                return out;
            }
            case TypoMechanism::DroppedLetter:
            {
                std::uniform_int_distribution<size_t> pos(0, word.size() - 1);
                std::string out = word;
                out.erase(pos(rng), 1);
                return out;
            }
            case TypoMechanism::AdjacentKey:
            {
                std::vector<size_t> letterPositions;
                for (size_t i = 0; i < word.size(); ++i)
                    if (QwertyNeighbors().count(static_cast<char>(std::tolower(static_cast<unsigned char>(word[i])))))
                        letterPositions.push_back(i);
                if (letterPositions.empty())
                    return word;
                std::uniform_int_distribution<size_t> pickPos(0, letterPositions.size() - 1);
                size_t i = letterPositions[pickPos(rng)];
                char lowerC = static_cast<char>(std::tolower(static_cast<unsigned char>(word[i])));
                const std::string& neighbors = QwertyNeighbors().at(lowerC);
                std::uniform_int_distribution<size_t> npick(0, neighbors.size() - 1);
                std::string out = word;
                out[i] = neighbors[npick(rng)];
                return out;
            }
            case TypoMechanism::DoubledLetter:
            {
                std::uniform_int_distribution<size_t> pos(0, word.size() - 1);
                std::string out = word;
                size_t i = pos(rng);
                out.insert(out.begin() + static_cast<long>(i), out[i]);
                return out;
            }
        }
        return word;
    }

    // care table: ~3% / ~1.5% / ~0.5% / 0%, per word. `correctionOut` is
    // cleared, then set to the pre-typo form of the *first* word this pass
    // actually alters -- the self-correction follow-up needs a single
    // corrected word -- left empty if no word ends up changed.
    std::string InjectTypos(const std::string& text, StyleBand band, const std::string& botName,
                             const std::string& senderName, std::mt19937& rng, std::string& correctionOut)
    {
        correctionOut.clear();

        float perWordRate = 0.0f;
        switch (band)
        {
            case StyleBand::Sloppy:  perWordRate = 0.03f;  break;
            case StyleBand::Loose:   perWordRate = 0.015f; break;
            case StyleBand::Careful: perWordRate = 0.005f; break;
            case StyleBand::Precise: perWordRate = 0.0f;   break;
        }
        if (perWordRate <= 0.0f)
            return text;

        std::vector<std::string> words = SplitWords(text);
        std::uniform_real_distribution<float> coin(0.0f, 1.0f);

        for (auto& word : words)
        {
            if (coin(rng) >= perWordRate)
                continue;
            if (IsProtectedToken(word, botName, senderName))
                continue;

            std::string trailingPunct;
            std::string core = SplitTrailingPunct(word, trailingPunct);
            if (core.size() < 2)
                continue;

            std::string typoed = ApplyOneTypoMechanism(core, rng);
            if (typoed == core)
                continue; // mechanism declined (e.g. AdjacentKey on a word with no letter it can hit)
            if (correctionOut.empty())
                correctionOut = core;
            word = typoed + trailingPunct;
        }
        return JoinWords(words);
    }

    // SplitMix64's finalizer. AzerothCore GUIDs are allocated from a small
    // sequential counter, so `std::hash<uint64_t>` (identity on libstdc++)
    // barely perturbs neighbouring GUIDs. This gives every input a
    // full-avalanche 64-bit spread regardless of how the platform's
    // std::hash<uint64_t> happens to behave.
    uint64_t MixBits64(uint64_t x)
    {
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBULL;
        x ^= x >> 31;
        return x;
    }

    uint64_t SeedFor(uint64_t botGuid, const std::string& text)
    {
        // Not cryptographic -- reproducibility for a given (bot, message)
        // pair is all this needs.
        uint64_t h = std::hash<std::string>{}(text);
        h ^= MixBits64(botGuid) + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        return h;
    }
}

float Hs_StyleCareForBot(uint64_t botGuid, float baselineCare, bool inCombat)
{
    // baselineCare comes from the bot's archetype (hs_archetype.h). A
    // +/-0.20 GUID jitter applies on top, so bots sharing an archetype still
    // sound like different people rather than reading identically.
    constexpr float kJitterWidth = 0.20f;

    // Negative offset in combat. Party chat during an encounter and a
    // positive offset for trade/recruitment posts are related cases with no
    // hook to read them from yet (this module only hooks /say and whisper --
    // hs_config.h). Combat is the one signal already reachable at the call
    // site (hs_handler.cpp's TryDispatch has the bot's Player*). -0.15 is a
    // starting guess, same footing as the rest of the archetype care table.
    constexpr float kCombatCareOffset = -0.15f;

    uint64_t h = MixBits64(botGuid ^ 0x9E3779B97F4A7C15ULL);
    float unit   = static_cast<float>(h % 100000ULL) / 100000.0f; // [0,1)
    float jitter = (unit * 2.0f - 1.0f) * kJitterWidth;            // [-0.20, 0.20)

    float care = baselineCare + jitter + (inCombat ? kCombatCareOffset : 0.0f);
    return std::clamp(care, 0.0f, 1.0f);
}

HsStyleResult Hs_ApplyStyle(uint64_t botGuid, const std::string& botName,
                             const std::string& senderName, const std::string& text,
                             const HsStyleContext& ctx)
{
    if (text.empty())
        return { text, "" };

    std::vector<std::string> spans;
    std::string working = ExtractProtectedSpans(text, spans);
    working = MaskLiteralPhrase(working, ctx.verbalTic, spans);

    working = StripLLMTells(working);
    if (working.empty())
        return { RestoreProtectedSpans(working, spans), "" };

    float care = Hs_StyleCareForBot(botGuid, ctx.baselineCare, ctx.inCombat);
    StyleBand band = BandForCare(care);

    std::mt19937 rng(static_cast<std::mt19937::result_type>(SeedFor(botGuid, text)));

    working = ApplyCasing(working, band, rng);
    working = ApplyTerminalPunctuation(working, band, rng);
    working = ApplyAbbreviation(working, band, ctx.abbrevOverrideChance, rng);

    std::string correction;
    working = InjectTypos(working, band, botName, senderName, rng, correction);

    return { RestoreProtectedSpans(working, spans), correction };
}
