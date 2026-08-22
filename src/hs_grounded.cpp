#include "hs_grounded.h"

#include <cctype>
#include <functional>
#include <vector>

namespace
{
    // Same SplitMix64 finalizer hs_style.cpp/hs_archetype.cpp/hs_reflex.cpp
    // use, for the same reason: AzerothCore GUIDs are sequential, so
    // std::hash<uint64_t> alone barely perturbs neighbouring GUIDs.
    // Duplicated locally rather than shared, matching this module's
    // existing per-file precedent.
    uint64_t MixBits64(uint64_t x)
    {
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBULL;
        x ^= x >> 31;
        return x;
    }

    // hash(botGuid, message text) -- §4.11 "seed per message, not per bot",
    // same idiom as hs_reflex.cpp's SeedForMessage (Plain family).
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

    std::string NormalizeWhitespace(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        bool lastWasSpace = true;
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

    struct KindPhrases
    {
        HsGroundedKind                  kind;
        const std::vector<const char*>* phrases;
    };

    const std::vector<const char*>& MountPhrases()
    {
        static const std::vector<const char*> phrases = {
            "nice mount", "nice mount, where from", "nice mount where from",
            "where'd you get that mount", "where did you get that mount",
            "what mount is that", "what's that mount", "whats that mount",
            "cool mount", "sweet mount", "what are you riding",
        };
        return phrases;
    }

    const std::vector<const char*>& LevelPhrases()
    {
        static const std::vector<const char*> phrases = {
            "what level are you", "what lvl are you", "what level r u", "what lvl r u",
            "ur level", "your level", "what's your level", "whats your level",
            "how strong are you",
        };
        return phrases;
    }

    const std::vector<const char*>& ZonePhrases()
    {
        static const std::vector<const char*> phrases = {
            "where are you", "where you at", "what zone are you in", "what zone is this",
            "where's this", "wheres this", "where is this", "what zone r u in",
        };
        return phrases;
    }

    const std::vector<const char*>& GuildPhrases()
    {
        static const std::vector<const char*> phrases = {
            "what guild are you in", "you in a guild", "your guild", "what guild",
            "you guilded", "are you in a guild",
        };
        return phrases;
    }

    const std::vector<const char*>& ProfessionPhrases()
    {
        static const std::vector<const char*> phrases = {
            "what professions do you have", "what professions you got",
            "what's your profession", "whats your profession", "any professions",
            "what do you craft", "you got any professions",
        };
        return phrases;
    }

    const std::vector<const char*>& GearPhrases()
    {
        static const std::vector<const char*> phrases = {
            "what are you wearing", "nice gear", "what's that gear", "whats that gear",
            "where'd you get that gear", "sweet gear", "nice armor",
        };
        return phrases;
    }

    // §4.20's own three named examples for the card-facts class.
    const std::vector<const char*>& CurrentGoalPhrases()
    {
        static const std::vector<const char*> phrases = {
            "what are you working on", "what are you up to", "what're you working on",
            "whatcha working on", "what are you doing tonight", "what's the plan",
            "whats the plan",
        };
        return phrases;
    }

    const std::vector<const char*>& PlayedSincePhrases()
    {
        static const std::vector<const char*> phrases = {
            "how long have you played", "how long you been playing", "how long have you been playing",
            "when did you start playing", "how long you played this game",
        };
        return phrases;
    }

    const std::vector<const char*>& AltPhrases()
    {
        static const std::vector<const char*> phrases = {
            "what do you main", "what's your main", "whats your main",
            "you got any alts", "what alts do you have", "what do you play besides this",
        };
        return phrases;
    }

    // §4.12/§4.20 step 16's three named recall examples.
    const std::vector<const char*>& RecallMetPhrases()
    {
        static const std::vector<const char*> phrases = {
            "do you remember me", "remember me", "do you know me",
            "have we met before", "have we met", "do we know each other",
        };
        return phrases;
    }

    const std::vector<const char*>& RecallDungeonPhrases()
    {
        static const std::vector<const char*> phrases = {
            "what did we run", "what did we run together", "what dungeon did we run",
            "what instance did we run", "what did we do together", "what have we run together",
        };
        return phrases;
    }

    const std::vector<const char*>& RecallGroupedPhrases()
    {
        static const std::vector<const char*> phrases = {
            "have we grouped before", "have we grouped up before", "have we grouped together before",
            "have we run together before", "have we played together before",
        };
        return phrases;
    }

    struct TemplatePart
    {
        const char* prefix;
        const char* suffix;
    };

    const std::vector<TemplatePart>& MountTemplates()
    {
        static const std::vector<TemplatePart> t = {
            { "it's a ", "" }, { "just my ", "" }, { "", ", nothing special" },
            { "picked up my ", " a while back" },
        };
        return t;
    }

    const std::vector<TemplatePart>& LevelTemplates()
    {
        static const std::vector<TemplatePart> t = {
            { "level ", "" }, { "I'm level ", "" }, { "", ", why" }, { "just hit ", "" },
        };
        return t;
    }

    const std::vector<TemplatePart>& ZoneTemplates()
    {
        static const std::vector<TemplatePart> t = {
            { "", "" }, { "", ", you?" }, { "just in ", "" }, { "hanging around ", "" },
        };
        return t;
    }

    const std::vector<TemplatePart>& GuildHasTemplates()
    {
        static const std::vector<TemplatePart> t = {
            { "I'm in ", "" }, { "", ", why" }, { "running with ", "" },
        };
        return t;
    }

    const std::vector<const char*>& GuildLacksResponses()
    {
        static const std::vector<const char*> r = {
            "nah, no guild atm", "not guilded right now", "solo for now",
        };
        return r;
    }

    const std::vector<TemplatePart>& ProfessionHasTemplates()
    {
        static const std::vector<TemplatePart> t = {
            { "", "" }, { "just ", "" }, { "picked up ", "" },
        };
        return t;
    }

    const std::vector<const char*>& ProfessionLacksResponses()
    {
        static const std::vector<const char*> r = {
            "haven't picked one up yet", "nothing right now", "nope, none atm",
        };
        return r;
    }

    const std::vector<TemplatePart>& GearHasTemplates()
    {
        static const std::vector<TemplatePart> t = {
            { "just this ", "" }, { "", ", nothing fancy" }, { "picked up this ", " a while back" },
        };
        return t;
    }

    const std::vector<const char*>& GearLacksResponses()
    {
        static const std::vector<const char*> r = {
            "not much really", "still gearing up",
        };
        return r;
    }

    const std::vector<TemplatePart>& CurrentGoalTemplates()
    {
        static const std::vector<TemplatePart> t = {
            { "", "" }, { "mostly ", "" }, { "honestly, ", "" }, { "right now, ", "" },
        };
        return t;
    }

    // `fact` here is one of the three enum values in
    // HsCardFacts::kPlayedSinceValues ("vanilla" | "bc" | "wrath") --
    // templated into natural phrasing rather than echoed as the raw
    // lowercase token.
    const std::vector<TemplatePart>& PlayedSinceTemplates()
    {
        static const std::vector<TemplatePart> t = {
            { "since ", "" }, { "playing since ", "" }, { "started back in ", "" }, { "", " baby" },
        };
        return t;
    }

    const std::vector<TemplatePart>& AltTemplates()
    {
        static const std::vector<TemplatePart> t = {
            { "I've got a ", " on the side" }, { "mostly this, but I dabble on a ", "" },
            { "a ", " when I need a break" }, { "", ", mostly" },
        };
        return t;
    }

    // Recall kinds -- `fact` unused for RecallMet/RecallGrouped (canned pool
    // on both sides, like Guild's lacks set); RecallDungeon wraps the
    // hside_memory row's own already-clean sentence (hs_memory.h's
    // Hs_BuildDungeonCompletedText), same "wrap the looked-up fact" shape as
    // Mount/Zone above.
    const std::vector<const char*>& RecallMetHasResponses()
    {
        static const std::vector<const char*> r = {
            "yeah, we've talked before", "of course I remember you", "we go back a bit",
        };
        return r;
    }

    const std::vector<const char*>& RecallMetLacksResponses()
    {
        static const std::vector<const char*> r = {
            "don't think we've met", "can't say I remember, sorry", "not that I recall",
        };
        return r;
    }

    const std::vector<TemplatePart>& RecallDungeonHasTemplates()
    {
        static const std::vector<TemplatePart> t = {
            { "", "" }, { "yeah, ", "" }, { "iirc, ", "" },
        };
        return t;
    }

    const std::vector<const char*>& RecallDungeonLacksResponses()
    {
        static const std::vector<const char*> r = {
            "can't think of anything we've run together", "don't remember running anything with you",
            "nothing comes to mind",
        };
        return r;
    }

    const std::vector<const char*>& RecallGroupedHasResponses()
    {
        static const std::vector<const char*> r = {
            "yeah, we have", "we have, actually", "yeah, a couple times",
        };
        return r;
    }

    const std::vector<const char*>& RecallGroupedLacksResponses()
    {
        static const std::vector<const char*> r = {
            "don't think so", "not that I recall", "not yet, I don't think",
        };
        return r;
    }
}

HsGroundedKind Hs_MatchGroundedQuestion(const std::string& trigger)
{
    std::string withPunct  = NormalizeWhitespace(ToLowerAscii(trigger));
    std::string corePhrase = StripOneTrailingMark(withPunct);

    static const std::vector<KindPhrases> table = {
        { HsGroundedKind::Mount,      &MountPhrases() },
        { HsGroundedKind::Level,      &LevelPhrases() },
        { HsGroundedKind::Zone,       &ZonePhrases() },
        { HsGroundedKind::Guild,      &GuildPhrases() },
        { HsGroundedKind::Profession, &ProfessionPhrases() },
        { HsGroundedKind::Gear,       &GearPhrases() },
        { HsGroundedKind::CurrentGoal, &CurrentGoalPhrases() },
        { HsGroundedKind::PlayedSince, &PlayedSincePhrases() },
        { HsGroundedKind::Alt,         &AltPhrases() },
        { HsGroundedKind::RecallMet,      &RecallMetPhrases() },
        { HsGroundedKind::RecallDungeon,  &RecallDungeonPhrases() },
        { HsGroundedKind::RecallGrouped,  &RecallGroupedPhrases() },
    };

    for (const KindPhrases& entry : table)
    {
        for (const char* phrase : *entry.phrases)
        {
            if (corePhrase == phrase)
                return entry.kind;
        }
    }
    return HsGroundedKind::None;
}

std::string Hs_BuildGroundedReply(HsGroundedKind kind, bool hasFact, const std::string& fact,
                                    uint64_t botGuid, const std::string& trigger)
{
    uint64_t seed = SeedForMessage(botGuid, trigger);

    auto pickTemplate = [&](const std::vector<TemplatePart>& templates) -> std::string
    {
        const TemplatePart& t = templates[seed % templates.size()];
        return t.prefix + fact + t.suffix;
    };
    auto pickResponse = [&](const std::vector<const char*>& responses) -> std::string
    {
        return responses[seed % responses.size()];
    };

    switch (kind)
    {
        case HsGroundedKind::Mount:      return pickTemplate(MountTemplates());
        case HsGroundedKind::Level:      return pickTemplate(LevelTemplates());
        case HsGroundedKind::Zone:       return pickTemplate(ZoneTemplates());
        case HsGroundedKind::Guild:      return hasFact ? pickTemplate(GuildHasTemplates()) : pickResponse(GuildLacksResponses());
        case HsGroundedKind::Profession: return hasFact ? pickTemplate(ProfessionHasTemplates()) : pickResponse(ProfessionLacksResponses());
        case HsGroundedKind::Gear:       return hasFact ? pickTemplate(GearHasTemplates()) : pickResponse(GearLacksResponses());
        case HsGroundedKind::CurrentGoal: return hasFact ? pickTemplate(CurrentGoalTemplates()) : "";
        case HsGroundedKind::PlayedSince: return hasFact ? pickTemplate(PlayedSinceTemplates()) : "";
        case HsGroundedKind::Alt:         return hasFact ? pickTemplate(AltTemplates()) : "";
        case HsGroundedKind::RecallMet:      return hasFact ? pickResponse(RecallMetHasResponses()) : pickResponse(RecallMetLacksResponses());
        case HsGroundedKind::RecallDungeon:  return hasFact ? pickTemplate(RecallDungeonHasTemplates()) : pickResponse(RecallDungeonLacksResponses());
        case HsGroundedKind::RecallGrouped:  return hasFact ? pickResponse(RecallGroupedHasResponses()) : pickResponse(RecallGroupedLacksResponses());
        case HsGroundedKind::None:       return "";
    }
    return "";
}
