#include "hs_identity.h"

#include <algorithm>
#include <cctype>

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

    const char* const kClassNames[] = {
        "warrior", "paladin", "hunter", "rogue", "priest",
        "death knight", "shaman", "mage", "warlock", "druid",
    };
    const size_t kClassNameCount = sizeof(kClassNames) / sizeof(kClassNames[0]);
}

namespace
{
    // Same four bands as hs_corpus.h's Hs_LevelBandFor -- duplicated rather
    // than depending on that header, keeping this file's dependency surface
    // at zero (hs_json.h and hs_gen_validate.h only).
    enum class Band { Low, Mid, High, Endgame };

    Band BandFor(uint8_t level)
    {
        if (level >= 80) return Band::Endgame;
        if (level >= 60) return Band::High;
        if (level >= 20) return Band::Mid;
        return Band::Low;
    }

    bool Contains(const char* const* values, size_t count, const std::string& value)
    {
        for (size_t i = 0; i < count; ++i)
            if (value == values[i])
                return true;
        return false;
    }

    // Each main_focus value's plausible bands. Authored, not derived --
    // see hs_identity.h's note that PLAN.md doesn't spell out this list.
    bool MainFocusAllowedInBand(const std::string& value, Band band)
    {
        if (value == "leveling")
            return band == Band::Low || band == Band::Mid;
        if (value == "gearing_up")
            return band == Band::Mid || band == Band::High || band == Band::Endgame;
        if (value == "dailies")
            return band == Band::Endgame;
        if (value == "raiding")
            return band == Band::High || band == Band::Endgame;
        if (value == "achievements")
            return band == Band::High || band == Band::Endgame;
        // pvp, professions, collecting -- credible at any level (§4.13's own
        // reasoning for PVP_CASUAL/TRADER: bg brackets from 10, gold is a
        // topic gate not an assignment gate).
        return value == "pvp" || value == "professions" || value == "collecting";
    }

    bool IsShortLiteral(const std::string& s, size_t maxLen)
    {
        if (s.size() > maxLen)
            return false;
        return s.find_first_of("\"`*[]{}") == std::string::npos;
    }

    // Format-only check for the two freeform fields (current_goal,
    // held_opinion) -- see hs_identity.h's note that acore_world existence-
    // checking is a named, accepted residual risk, not built here.
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

HsGenVerdict Hs_ValidateCardFacts(const hs_json& facts, uint8_t level, bool hasGuild)
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
    if (!Contains(HsCardFacts::kClassNames, HsCardFacts::kClassNameCount, alt))
        return { false, "alt_not_a_real_class_name" };

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
        "You are helping write a short persona note for a World of Warcraft: Wrath of the Lich "
        "King player character, to sit alongside their existing personality summary: \"You mostly "
        "talk about: " + archetypeTalksAbout + ".\" Write one or two short sentences, second "
        "person (\"You...\"), describing how this specific character comes across in chat -- their "
        "manner, not new facts about their life. No markdown, no emoji, no quotation marks, "
        "roughly 50 tokens.";
}

std::string Hs_BuildCardFactsPrompt(const std::string& archetypeTalksAbout, uint8_t level, bool hasGuild,
                                     const std::string& guildName)
{
    std::string prompt =
        "You are helping fill out a structured fact sheet for a World of Warcraft: Wrath of the "
        "Lich King player character (level " + std::to_string(static_cast<int>(level)) +
        ") whose personality summary is: \"You mostly talk about: " + archetypeTalksAbout + ".\" ";

    prompt += hasGuild
        ? ("This character is in a guild called \"" + guildName + "\". ")
        : std::string("This character is not currently in a guild. ");

    prompt +=
        "Reply with ONLY compact JSON (no markdown fences, no commentary) with exactly these eight "
        "string keys. Your reply must be a SINGLE LINE with ZERO line breaks anywhere in it -- not "
        "right after the opening brace, not between fields, not before the closing brace. Do not "
        "pretty-print or indent your reply. The eight keys:\n"
        "main_focus: one of leveling, gearing_up, dailies, raiding, pvp, professions, "
        "achievements, collecting -- pick one plausible for a level " + std::to_string(static_cast<int>(level)) +
        " character.\n"
        "current_goal: one short concrete near-term thing this character is working on right now.\n"
        "played_since: one of vanilla, bc, wrath.\n"
        "preferred_content: one of 5-mans, raids, pvp, solo.\n"
        "held_opinion: one short game opinion about a real instance, zone, or item.\n"
        "verbal_tic: a short literal word or phrase this character says often, or an empty string "
        "if none.\n"
        "guild_stance: \"" + std::string(hasGuild ? "guilded" : "unguilded") + "\" -- must match "
        "this character's actual guild status above.\n"
        "alt: the name of a different WoW class than this character's own, lowercase.";

    return prompt;
}
