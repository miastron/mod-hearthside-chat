#include "hs_identity.h"
#include "hs_class.h"
#include "hs_levelband.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace HsCardFacts
{
    const char* const kMainFocusValues[] = {
        "leveling", "gearing_up", "dailies", "raiding", "pvp", "professions", "achievements", "collecting",
    };
    const size_t kMainFocusCount = sizeof(kMainFocusValues) / sizeof(kMainFocusValues[0]);

    const char* const kPlayedSinceValues[] = { "vanilla", "bc", "wrath" };
    const size_t kPlayedSinceCount = sizeof(kPlayedSinceValues) / sizeof(kPlayedSinceValues[0]);

    const char* const kPreferredContentValues[] = { "5-mans", "raids", "pvp", "solo" };
    const size_t kPreferredContentCount = sizeof(kPreferredContentValues) / sizeof(kPreferredContentValues[0]);

    const char* const kGuildStanceValues[] = { "guilded", "unguilded" };
    const size_t kGuildStanceCount = sizeof(kGuildStanceValues) / sizeof(kGuildStanceValues[0]);

}

namespace
{
    // The same four bands Hs_LevelBandFor labels, off the same three
    // boundary constants (hs_levelband.h). This used to re-spell 20/60/80
    // with a comment saying the two had to stay in step; review item 25
    // made them share the numbers. The enum rather than the label strings
    // because the plausibility tables below switch on it.
    enum class Band { Low, Mid, High, Endgame };

    Band BandFor(uint8_t level)
    {
        if (level >= kHsLevelBandEndgameMin) return Band::Endgame;
        if (level >= kHsLevelBandHighMin)    return Band::High;
        if (level >= kHsLevelBandMidMin)     return Band::Mid;
        return Band::Low;
    }

    bool Contains(const char* const* values, size_t count, const std::string& value)
    {
        for (size_t i = 0; i < count; ++i)
            if (value == values[i])
                return true;
        return false;
    }

    // Each main_focus value's plausible bands, authored by hand, not
    // derived from an external spec.
    bool MainFocusAllowedInBand(const std::string& value, Band band)
    {
        // Review C11: High (60-79) is the Northrend levelling range in
        // WotLK, so "leveling" is not merely plausible there, it is the
        // single most likely answer for a character in that band. Rejecting
        // it produced a main_focus_not_plausible_for_level verdict the model
        // could hit repeatedly on the same bot, which is one of the two
        // deterministic card-generation failures behind the generator
        // livelock (review B2).
        if (value == "leveling")
            return band == Band::Low || band == Band::Mid || band == Band::High;
        if (value == "gearing_up")
            return band == Band::Mid || band == Band::High || band == Band::Endgame;
        if (value == "dailies")
            return band == Band::Endgame;
        if (value == "raiding")
            return band == Band::High || band == Band::Endgame;
        if (value == "achievements")
            return band == Band::High || band == Band::Endgame;
        // pvp, professions, collecting are credible at any level
        // (battlegrounds are available from level 10; gold is a topic gate,
        // not an assignment gate).
        return value == "pvp" || value == "professions" || value == "collecting";
    }

    bool IsShortLiteral(const std::string& s, size_t maxLen)
    {
        if (s.size() > maxLen)
            return false;
        return s.find_first_of("\"`*[]{}") == std::string::npos;
    }

    // Format-only check for the two freeform fields (current_goal,
    // held_opinion). Checking their subject against acore_world is a
    // named, accepted residual risk, not built here.
    bool IsPlausibleFreeform(const std::string& s)
    {
        if (s.empty() || s.size() > 120)
            return false;
        return s.find_first_of("\"`*[]{}") == std::string::npos;
    }
}

bool Hs_MainFocusPlausibleForLevel(const std::string& value, uint8_t level)
{
    return MainFocusAllowedInBand(value, BandFor(level));
}

std::string Hs_CardFactField(const hs_json& facts, const std::string& fieldName)
{
    if (!facts.is_object())
        return "";
    auto it = facts.find(fieldName);
    if (it == facts.end() || !it->is_string())
        return "";
    return it->get<std::string>();
}

std::string Hs_ExtractVerbalTic(const hs_json& facts)
{
    return Hs_CardFactField(facts, "verbal_tic");
}

HsGenVerdict Hs_ValidateCardFacts(const hs_json& facts, uint8_t level, bool hasGuild,
                                   const std::string& ownClassName)
{
    if (!facts.is_object())
        return { false, "not_an_object" };

    static const char* kRequiredKeys[] = {
        "main_focus", "current_goal", "played_since", "preferred_content",
        "held_opinion", "verbal_tic", "guild_stance", "alt",
    };
    for (auto const& key : kRequiredKeys)
    {
        auto it = facts.find(key);
        if (it == facts.end() || !it->is_string())
            return { false, std::string("missing_or_wrong_type:") + key };
    }

    std::string mainFocus = facts.at("main_focus").get<std::string>();
    if (!Contains(HsCardFacts::kMainFocusValues, HsCardFacts::kMainFocusCount, mainFocus))
        return { false, "main_focus_not_in_enum" };
    if (!Hs_MainFocusPlausibleForLevel(mainFocus, level))
        return { false, "main_focus_not_plausible_for_level" };

    std::string playedSince = facts.at("played_since").get<std::string>();
    if (!Contains(HsCardFacts::kPlayedSinceValues, HsCardFacts::kPlayedSinceCount, playedSince))
        return { false, "played_since_not_in_enum" };

    std::string preferredContent = facts.at("preferred_content").get<std::string>();
    if (!Contains(HsCardFacts::kPreferredContentValues, HsCardFacts::kPreferredContentCount, preferredContent))
        return { false, "preferred_content_not_in_enum" };

    std::string guildStance = facts.at("guild_stance").get<std::string>();
    if (!Contains(HsCardFacts::kGuildStanceValues, HsCardFacts::kGuildStanceCount, guildStance))
        return { false, "guild_stance_not_in_enum" };
    bool stanceIsGuilded = (guildStance == "guilded");
    if (stanceIsGuilded != hasGuild)
        return { false, "guild_stance_disagrees_with_actual_guild_row" };

    std::string alt = facts.at("alt").get<std::string>();
    if (!Contains(HsClass::kNames, HsClass::Count, alt))
        return { false, "alt_not_a_real_class_name" };
    // Review C10: Hs_BuildCardFactsPrompt asks for "a different WoW class
    // than this character's own", and nothing enforced it -- a card could
    // claim a warrior alt on a warrior. ownClassName is lowercase and drawn
    // from the same vocabulary as HsClass::kNames (hs_corpus.h's
    // Hs_ClassNameFor); empty means the caller could not determine it, in
    // which case the check is skipped rather than guessed.
    if (!ownClassName.empty() && alt == ownClassName)
        return { false, "alt_is_the_characters_own_class" };

    if (!IsPlausibleFreeform(facts.at("current_goal").get<std::string>()))
        return { false, "current_goal_bad_format" };
    if (!IsPlausibleFreeform(facts.at("held_opinion").get<std::string>()))
        return { false, "held_opinion_bad_format" };

    // verbal_tic may legitimately be empty ("no tic"), but if present it's a
    // short literal.
    if (!IsShortLiteral(facts.at("verbal_tic").get<std::string>(), 20))
        return { false, "verbal_tic_too_long_or_bad_chars" };

    return { true, "" };
}

HsGenVerdict Hs_ValidateVoiceBlock(const std::string& text)
{
    if (text.empty() || text.size() > 400)
        return { false, "length" };
    if (text.find_first_of("\"`*[]{}") != std::string::npos)
        return { false, "markdown_or_quote_chars" };
    return { true, "" };
}

std::string Hs_BuildVoiceBlockPrompt(const std::string& archetypeTalksAbout)
{
    return
        "You are helping write a short persona note for a World of Warcraft player character, to "
        "sit alongside their existing personality summary: \"You mostly "
        "talk about: " + archetypeTalksAbout + ".\" Write one or two short sentences, second "
        "person (\"You...\"), describing how this specific character comes across in chat -- their "
        "manner, not new facts about their life. No markdown, no emoji, no quotation marks, "
        "roughly 50 tokens.";
}

namespace
{
    // GBNF alternation of literal values: root ::= "a" | "b" | "c".
    // Every value here comes from a compile-time table in this file or
    // hs_class.h (lowercase words and hyphens, no quotes or backslashes), so
    // there is nothing to escape; a caller-supplied string would need it.
    std::string GrammarOneOf(const std::vector<std::string>& values)
    {
        if (values.empty())
            return "";
        std::string g = "root ::= ";
        for (size_t i = 0; i < values.size(); ++i)
        {
            if (i)
                g += " | ";
            g += "\"" + values[i] + "\"";
        }
        return g;
    }
}

char const* Hs_CardFactTrigger()
{
    return "Answer in one short line. Reply with only the answer.";
}

HsCardFactAsk Hs_CardFactAsk(uint32_t index, const std::string& archetypeTalksAbout, uint8_t level,
                              bool hasGuild, const std::string& guildName,
                              const std::string& ownClassName)
{
    // Shared lead-in. Short on purpose: the long fact-sheet framing the old
    // single-call prompt used is what pushed the model out of the register it
    // can actually answer in.
    const std::string who =
        "You are a level " + std::to_string(static_cast<int>(level)) +
        " World of Warcraft player. You mostly talk about: " + archetypeTalksAbout + ". ";

    switch (index)
    {
        case 0:
        {
            // Only the focuses that are plausible at this level reach the
            // grammar, so main_focus_not_plausible_for_level (review C11)
            // cannot be generated. The prompt still names them: the model
            // picks better when it can see the options, and the grammar is
            // what makes the pick binding.
            std::vector<std::string> allowed;
            for (size_t i = 0; i < HsCardFacts::kMainFocusCount; ++i)
                if (Hs_MainFocusPlausibleForLevel(HsCardFacts::kMainFocusValues[i], level))
                    allowed.emplace_back(HsCardFacts::kMainFocusValues[i]);

            std::string list;
            for (size_t i = 0; i < allowed.size(); ++i)
                list += (i ? ", " : "") + allowed[i];

            return { "main_focus",
                     who + "What are you mainly doing in the game these days? One of: " + list + ".",
                     GrammarOneOf(allowed), "" };
        }
        case 1:
            return { "current_goal", who +
                "What is the one thing you are working on right now?", "", "" };
        case 2:
        {
            std::vector<std::string> allowed(HsCardFacts::kPlayedSinceValues,
                                             HsCardFacts::kPlayedSinceValues + HsCardFacts::kPlayedSinceCount);
            return { "played_since",
                     who + "When did you start playing? One of: vanilla, bc, wrath.",
                     GrammarOneOf(allowed), "" };
        }
        case 3:
        {
            std::vector<std::string> allowed(HsCardFacts::kPreferredContentValues,
                                             HsCardFacts::kPreferredContentValues + HsCardFacts::kPreferredContentCount);
            return { "preferred_content",
                     who + "What kind of content do you like best? One of: 5-mans, raids, pvp, solo.",
                     GrammarOneOf(allowed), "" };
        }
        case 4:
            return { "held_opinion", who +
                "Give one opinion you hold about a dungeon, raid, zone or item in the game.",
                "", "" };
        case 5:
            // Freeform by design: a tic is the one card fact that has to be
            // this character's own words, so there is no vocabulary to
            // constrain it to. "none" is normalised to empty by
            // Hs_NormalizeCardFactValue.
            return { "verbal_tic", who +
                "Is there a short word or phrase you say a lot in chat? Answer with just that "
                "word or phrase, or the word none.", "", "" };
        case 6:
            // Not asked. The guild row is already in hand; a model restating
            // it can only disagree with it, and Hs_ValidateCardFacts rejects
            // the whole card when it does.
            (void)guildName;
            return { "guild_stance", "", "", hasGuild ? "guilded" : "unguilded" };
        default:
        {
            // The bot's own class is dropped from the grammar, so
            // alt_is_the_characters_own_class (review C10) cannot be
            // generated either.
            std::vector<std::string> allowed;
            for (size_t i = 0; i < HsClass::Count; ++i)
                if (ownClassName.empty() || ownClassName != HsClass::kNames[i])
                    allowed.emplace_back(HsClass::kNames[i]);

            std::string p = who + "Name one other class you also play, lowercase.";
            if (!ownClassName.empty())
                p += " It must not be " + ownClassName + ".";
            return { "alt", p, GrammarOneOf(allowed), "" };
        }
    }
}

std::string Hs_NormalizeCardFactValue(uint32_t index, std::string raw)
{
    auto trim = [](std::string& s)
    {
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    };

    // Collapse any stray newline first: everything below assumes one line.
    for (char& c : raw)
        if (c == '\n' || c == '\r')
            c = ' ';
    trim(raw);

    // "main_focus: raiding" -> "raiding". The model echoes the key back often
    // enough to be worth handling, and only when the prefix is exactly this
    // field's own key, so a colon inside a real answer survives.
    const std::string key = Hs_CardFactAsk(index, "", 1, false, "").key;
    if (raw.size() > key.size() + 1 && raw.compare(0, key.size(), key) == 0
        && raw[key.size()] == ':')
    {
        raw = raw.substr(key.size() + 1);
        trim(raw);
    }

    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
    {
        raw = raw.substr(1, raw.size() - 2);
        trim(raw);
    }
    while (!raw.empty() && (raw.back() == '.' || raw.back() == '!'))
    {
        raw.pop_back();
        trim(raw);
    }

    // The four enum fields answer from a fixed vocabulary, so case and the
    // space/underscore difference are transcription noise rather than a wrong
    // answer. The freeform fields keep whatever the model wrote.
    const bool isEnum = (index == 0 || index == 2 || index == 3 || index == 6);
    if (isEnum)
    {
        for (char& c : raw)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (c == ' ')
                c = '_';
        }
    }

    // verbal_tic: "none" is how the prompt invites an empty answer, and the
    // validator treats empty as "no tic".
    //
    // An answer that runs long is treated the same way, and that is the point
    // rather than a shortcut. A tic is one or two words; when this model
    // instead writes a sentence ("explain the mechanic, i'm always unsure")
    // what it has actually told us is that this character has no catchphrase,
    // which is a legal value. The alternative was a length-bounded grammar,
    // and it was measured and rejected: capping the sampler at 20 characters
    // stops it mid-word and stores "check it before youa" as a bot's
    // catchphrase, which is worse than having none. Failing the card was the
    // other alternative, and it would fail most cards over the least
    // important of the eight fields.
    if (index == 5)
    {
        std::string lower = raw;
        for (char& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "none" || lower == "no" || lower == "nothing")
            return "";
        if (raw.size() > 20 || raw.find_first_of("\"`*[]{}") != std::string::npos)
            return "";
    }

    if (index == 7)
    {
        for (char& c : raw)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return raw;
}
