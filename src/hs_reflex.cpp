#include "hs_reflex.h"

#include <cctype>
#include <functional>
#include <regex>
#include <vector>

namespace
{
    // Same SplitMix64 finalizer hs_style.cpp/hs_archetype.cpp use, for the
    // same reason: AzerothCore GUIDs come from a small sequential counter,
    // so std::hash<uint64_t> alone barely perturbs neighbouring GUIDs.
    // Duplicated locally rather than shared, matching this module's existing
    // per-file precedent (hs_archetype.cpp carries its own copy too).
    uint64_t MixBits64(uint64_t x)
    {
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBULL;
        x ^= x >> 31;
        return x;
    }

    constexpr uint64_t kBotQuestionSalt   = 0x9E6B4A1D7F0C3358ULL;
    constexpr uint64_t kPersonalProbeSalt = 0x51F0A8D3C6E29B47ULL;

    // hash(botGuid, senderGuid, salt) -- independent salts keep the two
    // per-player-consistent families from picking correlated indices for
    // the same bot/player pair.
    uint64_t SeedForPlayer(uint64_t botGuid, uint64_t senderGuid, uint64_t salt)
    {
        uint64_t h = MixBits64(botGuid ^ salt);
        h ^= MixBits64(senderGuid) + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        return h;
    }

    // hash(botGuid, message text) -- seeds per message rather than per bot,
    // same idiom hs_style.cpp's SeedFor uses.
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

    // Trims outer whitespace and collapses internal whitespace runs to a
    // single space. Punctuation is left alone -- callers decide how much of
    // it to strip, since the BotQuestion family needs to keep a literal
    // trailing '?' for the bare "bot?" case, while the Plain family strips
    // it freely.
    std::string NormalizeWhitespace(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        bool lastWasSpace = true; // skips leading whitespace too
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

    // Collapses any run of 3+ identical characters down to one ("loooool"
    // -> "lol", "tyyyy" -> "ty") and strips any trailing run of !?. -- the
    // Plain family's tolerance for how a one-word reflex actually gets
    // typed. Not used by BotQuestion/PersonalProbe, which stay strict: a
    // false positive there is far worse than a miss.
    std::string CompressForPlainMatch(const std::string& s)
    {
        static const std::regex kRepeatRun(R"((.)\1{2,})");
        std::string collapsed = std::regex_replace(s, kRepeatRun, "$1");

        size_t end = collapsed.size();
        while (end > 0 && (collapsed[end - 1] == '!' || collapsed[end - 1] == '?' || collapsed[end - 1] == '.'))
            --end;
        return collapsed.substr(0, end);
    }

    // Strips at most one trailing '?', '!' or '.' -- BotQuestion/
    // PersonalProbe's tolerance for "how old are you?" vs "how old are
    // you", without the Plain family's aggressive repeat-collapsing, which
    // would turn "bot??" into "bot?" and blur the bare-"bot?" special case.
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

    struct PlainEntry
    {
        const char*              trigger;
        std::vector<const char*> responses;
    };

    const std::vector<PlainEntry>& PlainTable()
    {
        // inv/sum are the two triggers that ask the bot for an action
        // (invite, summon) this module cannot actually perform (it governs
        // speech only), so their replies stay honest and noncommittal
        // rather than promising a follow-up the bot will never deliver.
        static const std::vector<PlainEntry> table = {
            { "gz",  { "ty!", "thanks!", "appreciate it" } },
            { "ty",  { "np", "np!", "yw" } },
            { "wb",  { "ty", "thx", "good to be back" } },
            { "lol", { "lol", "haha", "right?" } },
            { "inv", { "can't inv rn, sry", "not able to inv atm", "sry, can't rn" } },
            { "sum", { "can't sum rn, sry", "no way to sum atm", "sry, can't help with that rn" } },
        };
        return table;
    }

    const std::vector<std::string>& BotQuestionPhrases()
    {
        // Must never match bare "bot". Every entry here is multi-word; the
        // single-word "bot?" case is handled separately in Hs_MatchReflex
        // and requires the literal question mark. So a standalone "bot" /
        // "ah bot" / "botting" can never equal any BotQuestion match under
        // whole-message comparison.
        static const std::vector<std::string> phrases = {
            "are you a bot", "r u a bot", "are u a bot", "u a bot", "u bot",
            "is this an npc", "is this a bot",
            "are you an npc", "r u an npc",
            "you a bot", "you're a bot", "ur a bot",
            "are you human", "r u human", "are you a real person",
            "bot or human", "human or bot",
            "are you real", "r u real",
        };
        return phrases;
    }

    const std::vector<std::string>& PersonalProbePhrases()
    {
        // Core personal-probe questions -- "where are you from", "what do
        // you do", "how old are you", "m or f" -- plus close variants.
        static const std::vector<std::string> phrases = {
            "where are you from", "where you from", "where r u from",
            "what do you do", "what do you do irl", "what do you do for a living",
            // Deliberately no bare "age" -- same multi-word rule
            // BotQuestionPhrases states above, and for the same reason.
            // Matching is whole-message, so a bare entry fires on a message
            // that is exactly that word, and a one-word "age" is far more
            // often a level-bracket question or a sentence fragment than a
            // personal probe. A false positive here is expensive: the reflex
            // sets handled = true (hs_handler.cpp), short-circuiting the
            // grounded answer and the LLM tier entirely, so the player gets
            // "that's classified ;)" as a non sequitur and nothing else.
            "how old are you", "how old r u", "ur age", "your age", "whats your age", "what's your age",
            "m or f", "male or female", "boy or girl",
            "whats your name", "what's your name", "ur real name", "your real name",
            "whats ur discord", "what's your discord", "got discord", "add me on discord",
            "where do you live", "where u live",
            "you got a mic", "can you voice chat",
            "how tall are you", "what do you look like",
        };
        return phrases;
    }

    const std::vector<const char*>& BotQuestionResponses(HsBotQuestionMode mode)
    {
        // Wink is the honest non-answer: neither confirms nor denies.
        // Deflect is its more evasive subset ("huh?"/"what?" only). Admit is
        // the operator's explicit opt-in to being straightforward.
        static const std::vector<const char*> wink    = { "maybe!", "shh... don't tell anyone", "huh?", "what?", "who's asking", "wouldn't you like to know" };
        static const std::vector<const char*> deflect = { "huh?", "what?", "hm?" };
        static const std::vector<const char*> admit   = { "yeah, I'm a bot", "yep, this one's a bot", "yep, bot confirmed" };
        switch (mode)
        {
            case HsBotQuestionMode::Deflect: return deflect;
            case HsBotQuestionMode::Admit:   return admit;
            default:                          return wink; // Wink; Silent never reads this
        }
    }

    const std::vector<const char*>& PersonalProbeResponses()
    {
        // A vague deflection, a joke, a subject change, and an occasional
        // no-reply reads as a person; a rule reads as a rule. One shared
        // pool across every probe question -- the honest non-answer is the
        // same regardless of which personal question triggered it. The
        // empty entry is the no-reply member: 1 of 9, occasional, not the
        // rule.
        static const std::vector<const char*> pool = {
            "eh, does it matter", "long story", "who's asking",
            "that's classified ;)", "anyway, so...", "next question",
            "ask me later", "focus, we've got mobs to kill",
            "",
        };
        return pool;
    }
}

HsBotQuestionMode Hs_ParseBotQuestionMode(const std::string& value)
{
    if (value == "deflect") return HsBotQuestionMode::Deflect;
    if (value == "silent")  return HsBotQuestionMode::Silent;
    if (value == "admit")   return HsBotQuestionMode::Admit;
    return HsBotQuestionMode::Wink; // default, and the fallback for anything unrecognised
}

HsReflexMatch Hs_MatchReflex(const std::string& trigger, uint64_t botGuid, uint64_t senderGuid,
                              HsBotQuestionMode botQuestionMode)
{
    std::string withPunct  = NormalizeWhitespace(ToLowerAscii(trigger));
    std::string corePhrase = StripOneTrailingMark(withPunct);

    // ---- "are you a bot?" -- checked first: it is the module's
    // most-scrutinised line and the narrowest, most specific match. ----
    bool isBotQuestion = (withPunct == "bot?");
    if (!isBotQuestion)
    {
        for (const std::string& phrase : BotQuestionPhrases())
        {
            if (corePhrase == phrase)
            {
                isBotQuestion = true;
                break;
            }
        }
    }
    if (isBotQuestion)
    {
        HsReflexMatch match;
        match.kind = HsReflexKind::BotQuestion;
        if (botQuestionMode != HsBotQuestionMode::Silent)
        {
            const std::vector<const char*>& responses = BotQuestionResponses(botQuestionMode);
            uint64_t seed = SeedForPlayer(botGuid, senderGuid, kBotQuestionSalt);
            match.text = responses[seed % responses.size()];
        }
        return match;
    }

    // ---- personal-probe deflection ----
    for (const std::string& phrase : PersonalProbePhrases())
    {
        if (corePhrase == phrase)
        {
            HsReflexMatch match;
            match.kind = HsReflexKind::PersonalProbe;
            const std::vector<const char*>& responses = PersonalProbeResponses();
            uint64_t seed = SeedForPlayer(botGuid, senderGuid, kPersonalProbeSalt);
            match.text = responses[seed % responses.size()];
            return match;
        }
    }

    // ---- plain reflex vocabulary (gz/ty/inv/sum/lol/wb) ----
    std::string plainCore = CompressForPlainMatch(withPunct);
    for (const PlainEntry& entry : PlainTable())
    {
        if (plainCore == entry.trigger)
        {
            HsReflexMatch match;
            match.kind = HsReflexKind::Plain;
            uint64_t seed = SeedForMessage(botGuid, trigger);
            match.text = entry.responses[seed % entry.responses.size()];
            return match;
        }
    }

    return HsReflexMatch{}; // kind stays None -- caller falls through
}
