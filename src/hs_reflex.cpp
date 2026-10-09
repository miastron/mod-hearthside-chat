#include "hs_reflex.h"
#include "hs_hash.h"
#include "hs_text.h"

#include <cctype>
#include <functional>
#include <regex>
#include <unordered_map>
#include <vector>

namespace
{
    // Same SplitMix64 finalizer hs_style.cpp/hs_archetype.cpp use, for the
    // same reason: AzerothCore GUIDs come from a small sequential counter,
    // so std::hash<uint64_t> alone barely perturbs neighbouring GUIDs.
    // Duplicated locally rather than shared, matching this module's existing
    // per-file precedent (hs_archetype.cpp carries its own copy too).

    constexpr uint64_t kBotQuestionSalt   = 0x9E6B4A1D7F0C3358ULL;
    constexpr uint64_t kPersonalProbeSalt = 0x51F0A8D3C6E29B47ULL;

    // hash(botGuid, senderGuid, salt). Independent salts keep the two
    // per-player-consistent families from picking correlated indices for
    // the same bot/player pair.
    uint64_t SeedForPlayer(uint64_t botGuid, uint64_t senderGuid, uint64_t salt)
    {
        uint64_t h = HsHash::Hs_MixBits64(botGuid ^ salt);
        h ^= HsHash::Hs_MixBits64(senderGuid) + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        return h;
    }

    // Collapses any run of 3+ identical characters down to one ("loooool"
    // -> "lol", "tyyyy" -> "ty") and strips any trailing run of !?. (the
    // Plain family's tolerance for how a one-word reflex actually gets
    // typed). Not used by BotQuestion/PersonalProbe, which stay strict: a
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
            // Bare greetings and goodbyes, 2026-10-07. The tuned model had two
            // "hi" rows in 2645 and answered one as if it were "how are you"
            // and the next by announcing its archetype ("fine, lets get to the
            // raid tonight, no chatting"). A player who whispers "hey" expects
            // "hey" back, and anything said next goes to the model as usual.
            { "hi",    { "hey", "hi", "yo", "o/", "heya" } },
            { "hey",   { "hey", "hi", "yo", "o/", "sup" } },
            { "hello", { "hey", "hi", "hello", "o/" } },
            { "hiya",  { "hey", "hi", "o/" } },
            { "heya",  { "hey", "heya", "o/" } },
            { "yo",    { "yo", "sup", "hey" } },
            { "o/",    { "o/", "hey", "yo" } },
            { "sup",   { "not much, u?", "nm, u?", "not much", "hey, not much" } },
            { "bye",   { "cya", "later", "bye", "take care" } },
            { "cya",   { "cya", "later", "o/" } },
            // Scored 0.72 and 0.39 of 2 from the model at temperature 0.5
            // (Tests/whisper_probe.py, blind-judged, 2026-10-07): "thanks"
            // drew "gz" from half the archetypes, and "what?" an answer to a
            // question nobody asked.
            { "thanks",    { "np", "yw", "no prob", "np!" } },
            { "thx",       { "np", "yw", "no prob" } },
            { "thank you", { "np", "yw", "no problem" } },
            { "what",      { "nvm", "nvm lol", "nothing, nvm" } },
            { "brb",       { "k", "ok", "kk" } },
            { "afk",       { "k", "ok" } },
            { "gtg",       { "cya", "later", "o/" } },
            { "gn",        { "gn", "night", "cya" } },
            // A random bot holds no guild rank that can invite, and the model
            // agreed to anyway ("sure, just ask me", realm 2026-10-07).
            { "can i join your guild",   { "cant ginv, not an officer", "no invite rights sry", "not an officer, cant inv u" } },
            { "can i join ur guild",     { "cant ginv, not an officer", "no invite rights sry", "not an officer, cant inv u" } },
            { "invite me to your guild", { "cant ginv, not an officer", "no invite rights sry", "not an officer, cant inv u" } },
            { "invite me to ur guild",   { "cant ginv, not an officer", "no invite rights sry", "not an officer, cant inv u" } },
            { "ginv",                    { "cant ginv, not an officer", "no invite rights sry" } },
            { "ginv pls",                { "cant ginv, not an officer", "no invite rights sry" } },
            { "guild invite",            { "cant ginv, not an officer", "no invite rights sry" } },
            { "huh",       { "nvm", "nvm lol", "nothing" } },
            // A compliment, not a question: these phrases also sit in
            // hside_grounded_question's GEAR set, whose answers are written
            // for "what are you wearing" ("just this Blade of Misfortune"),
            // and reflex runs first.
            { "nice gear",    { "ty", "thanks", "ty, took a while", "ty lol" } },
            { "sweet gear",   { "ty", "thanks", "ty lol" } },
            { "cool gear",    { "ty", "thanks", "ty lol" } },
            { "sick gear",    { "ty", "thanks", "ty lol" } },
            { "awesome gear", { "ty", "thanks", "ty lol" } },
            { "nice armor",   { "ty", "thanks", "ty lol" } },
            { "nice set",     { "ty", "thanks", "ty, took a while" } },
            { "love that gear", { "ty", "thanks", "ty lol" } },
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
        // Core personal-probe questions ("where are you from", "what do
        // you do", "how old are you", "m or f") plus close variants.
        static const std::vector<std::string> phrases = {
            "where are you from", "where you from", "where r u from",
            "what do you do", "what do you do irl", "what do you do for a living",
            // Deliberately no bare "age": same multi-word rule
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
        // pool across every probe question: the honest non-answer is the
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
    std::string withPunct  = HsText::Hs_NormalizeWhitespace(HsText::Hs_ToLowerAscii(trigger));
    // One trailing '?'/'!'/'.' only, not a run: BotQuestion/PersonalProbe's
    // tolerance for "how old are you?" vs "how old are you", without the
    // Plain family's aggressive repeat-collapsing below, which would turn
    // "bot??" into "bot?" and blur the bare-"bot?" special case.
    std::string corePhrase = HsText::Hs_StripOneTrailingMark(withPunct);

    // ---- "are you a bot?" (checked first: the module's most-scrutinised
    // line and the narrowest, most specific match) ----
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

    // ---- plain reflex vocabulary (gz/ty/inv/sum/lol/wb, greetings) ----
    std::string plainCore = CompressForPlainMatch(withPunct);
    for (const PlainEntry& entry : PlainTable())
    {
        if (plainCore == entry.trigger)
        {
            HsReflexMatch match;
            match.kind = HsReflexKind::Plain;
            uint64_t seed = HsHash::Hs_SeedForMessage(botGuid, trigger);
            match.text = entry.responses[seed % entry.responses.size()];
            return match;
        }
    }

    // ---- conversation closers: nobody answers "ok" ----
    static const char* const kClosers[] = { "ok", "k", "kk", "okay", "cool", "nice", "alright", "np", "no problem",
                                             "sure", "yep", "ya", "yeah", "kk ty", "ok ty", "ok thanks" };
    for (const char* closer : kClosers)
    {
        if (plainCore == closer)
        {
            HsReflexMatch match;
            match.kind = HsReflexKind::Plain; // matched, text empty: handled as silence
            return match;
        }
    }

    return HsReflexMatch{}; // kind stays None; caller falls through
}

std::string Hs_ExpandChatShorthand(const std::string& rawText)
{
    static const std::unordered_map<std::string, const char*> kExpand = {
        { "wyd", "what are you doing" }, { "wbu", "what about you" }, { "hbu", "how about you" },
        { "hru", "how are you" }, { "wru", "where are you" }, { "wya", "where are you" },
        { "sup", "what's up" }, { "u", "you" }, { "ur", "your" }, { "r", "are" },
        { "idk", "i don't know" }, { "idc", "i don't care" }, { "dunno", "don't know" },
        { "ty", "thanks" }, { "thx", "thanks" }, { "np", "no problem" }, { "pls", "please" },
        { "plz", "please" }, { "ppl", "people" }, { "rn", "right now" }, { "atm", "at the moment" },
        { "tbh", "to be honest" }, { "ngl", "not gonna lie" }, { "imo", "in my opinion" },
        { "lfg", "looking for group" }, { "lf", "looking for" }, { "lvl", "level" },
        { "lvling", "leveling" }, { "brb", "be right back" }, { "gtg", "got to go" },
        { "afk", "away" }, { "nvm", "never mind" }, { "sry", "sorry" }, { "cuz", "because" },
        { "bc", "because" }, { "ofc", "of course" }, { "gz", "congrats" }, { "grats", "congrats" },
        { "k", "ok" }, { "kk", "ok" }, { "wanna", "want to" }, { "gonna", "going to" },
        { "whats", "what's" }, { "hows", "how's" }, { "wheres", "where's" }, { "im", "i'm" },
        { "dont", "don't" }, { "cant", "can't" },
    };

    std::string const text = HsText::Hs_StripChatLinks(rawText);
    std::string out;
    out.reserve(text.size() + 16);
    size_t i = 0;
    while (i < text.size())
    {
        if (!std::isalpha(static_cast<unsigned char>(text[i])))
        {
            out.push_back(text[i++]);
            continue;
        }
        size_t j = i;
        while (j < text.size() && (std::isalpha(static_cast<unsigned char>(text[j])) || text[j] == '\''))
            ++j;
        std::string word = text.substr(i, j - i);
        auto it = kExpand.find(HsText::Hs_ToLowerAscii(word));
        out += (it != kExpand.end()) ? it->second : word;
        i = j;
    }
    return out;
}
