#ifndef MOD_HS_CORPUS_H
#define MOD_HS_CORPUS_H

#include "hs_channel.h"

#include <cstdint>
#include <string>

class Player;

// When a surface's MaxTier ceiling permits corpus but not inference, this
// answers instead of falling straight to silence. Weighted anti-repeat pick
// from a category the bot's class/level qualifies for, synchronous like
// TryReflex/TryGrounded in hs_handler.cpp -- zero GPU work, so no queue, no
// bucket, no cooldown; only the ceiling->inference branch touches the token
// bucket.
//
// Scoped to channel IS NULL categories -- the /say and direct-reply set
// (channel_* categories are ambient content for the global-channel surface,
// selected via Hs_SelectChannelLine below instead). Covers every tag axis a
// seeded category now uses: none, class, level_band, faction, zone.
//
// `botFaction`: Player::GetTeamId() (0 = Alliance, 1 = Horde), the same
// faction signal every other faction check in this module already uses
// (hs_handler.cpp, hs_opener.cpp, hs_engagement.cpp, hs_script.cpp).
// `botZoneId`: Player::GetZoneId(), the same id hs_corpus.cpp's own
// Hs_BuildPlaceholderContext already resolves for %zone.
//
// Returns empty if no eligible category has a matching row (schema not
// installed, or nothing fits this bot's class/level/faction/zone) --
// caller falls through to silence like any other "nothing to say" path.
//
// `hasActiveCard`: a card_gated=1 category is only eligible when true --
// its rows use %main_focus/%current_goal, which only resolve for a carded
// bot. False (the overwhelming majority of bots) simply removes those
// categories from the eligible set, same as the other tag filtering.
std::string Hs_SelectCorpusLine(uint8_t botClass, uint8_t botLevel, uint8_t botFaction, uint32_t botZoneId, bool hasActiveCard);

// An opener fires off a specific shared-context trigger (hs_opener.cpp)
// that already knows which category applies -- "group formed" wants
// opener_group_formed, not a random pick among all eligible categories.
// Same anti-repeat pick and exposure bookkeeping as Hs_SelectCorpusLine,
// scoped to one named category. Returns empty if the category doesn't
// exist, isn't flagged is_opener, or has no row matching this bot's
// class/level/faction/zone. No is_opener=1 category is faction/zone-axis
// today, but the plumbing carries the signal regardless -- same shape as
// the class/level_band tags an opener category doesn't use either.
std::string Hs_SelectOpenerLine(const std::string& categoryName, uint8_t botClass, uint8_t botLevel,
                                 uint8_t botFaction, uint32_t botZoneId);

// §4.17: the channel_* categories' selection path (hs_corpus_category.sql's
// `channel` column), previously unwired -- same anti-repeat pick and
// exposure bookkeeping as Hs_SelectCorpusLine, scoped to categories tagged
// for this channel instead of the channel-IS-NULL /say set. Only
// Trade/General/World have any channel_* rows seeded today; a kind with none
// simply returns empty, same as any other "nothing eligible" case. Called
// from hs_handler.cpp's Channel* hook (§4.17), never for a channel whose
// policy is Off.
std::string Hs_SelectChannelLine(HsChannelKind kind, uint8_t botClass, uint8_t botLevel,
                                  uint8_t botFaction, uint32_t botZoneId);

// PLAN-AMBIENT.md §4: party/raid ambient content. A third selection scope
// alongside the two above, and it needs to be its own function rather than
// an HsChannelKind added to Hs_SelectChannelLine -- party and raid are not
// global channels. They have no Channel* to resolve, no per-channel policy
// row, and no membership beyond the bot's own Group, so folding them into
// HsChannelKind would put two values into that enum that every other
// consumer of it (the policy table, Hs_ResolveChannelForDelivery,
// hs_handler.cpp's Channel* hook) would have to special-case away.
//
// Reuses hside_corpus_category's existing `channel` column, whose values
// extend from trade|general|world to also include party|raid. Nothing else
// has to change for that: Hs_SelectCorpusLine's `channel IS NULL` filter
// already excludes every non-NULL value, so the new rows stay out of the
// /say and direct-reply pool by the same mechanism the channel_* categories
// already do.
//
// The register is genuinely different from a /say musing, which is why these
// get their own categories rather than reusing the five chat_* ones: a /say
// line is overheard by whoever happens to be nearby, while a party line is
// addressed to four specific people who are doing something together.
//
// `isRaid` selects the raid categories over the party ones. Returns empty
// if nothing is eligible, same contract as the two functions above.
std::string Hs_SelectGroupAmbientLine(bool isRaid, uint8_t botClass, uint8_t botLevel,
                                       uint8_t botFaction, uint32_t botZoneId);

// The four level_band_tag labels used by chat_levelband_musing's seeded
// rows: low 1-19, mid 20-59, high 60-79, endgame 80 (the level cap) --
// lined up with WotLK's own pacing (Outland opens at 58, Northrend at 68,
// raiding/dailies only exist at the level-80 cap) rather than an even
// split.
//
// Inline and header-only (no hs_corpus.cpp dependency, which pulls in
// AzerothCore's DatabaseEnv.h) so a standalone test harness can include
// just this header, same pattern as hs_tier.h's HsParseTier/HsTierAllows.
inline std::string Hs_LevelBandFor(uint8_t level)
{
    if (level >= 80) return "endgame";
    if (level >= 60) return "high";
    if (level >= 20) return "mid";
    return "low";
}

// Card-only placeholder resolution: literal substring replacement, not a
// template engine -- there are exactly two card-only placeholders
// (hs_gen_validate.cpp's kCardOnlyPlaceholders), and a card-gated category
// is only ever selected for a bot that actually has both. `mainFocus`/
// `currentGoal` empty is a no-op for that token (defensive; shouldn't
// happen given the card_gated gating above). Pure and header-only, no
// AzerothCore dependency, same reasoning as Hs_LevelBandFor.
inline std::string Hs_ResolveCardPlaceholders(std::string text, const std::string& mainFocus,
                                               const std::string& currentGoal)
{
    auto replaceAll = [](std::string& s, const std::string& token, const std::string& value)
    {
        if (value.empty())
            return;
        size_t pos = 0;
        while ((pos = s.find(token, pos)) != std::string::npos)
        {
            s.replace(pos, token.size(), value);
            pos += value.size();
        }
    };
    replaceAll(text, "%main_focus", mainFocus);
    replaceAll(text, "%current_goal", currentGoal);
    return text;
}

// The universal corpus placeholders (hs_gen_validate.cpp's
// kUniversalPlaceholders) that resolve off any bot's own character row, as
// opposed to the two card-only tokens Hs_ResolveCardPlaceholders handles
// above. An empty field means the bot has nothing true to put here (e.g.
// unguilded, empty bags), which drops the line rather than inventing a
// value (§4.13).
struct HsPlaceholderContext
{
    std::string itemLink;   // chat hyperlink to an item actually in the bot's bags
    std::string questLink;  // chat hyperlink to a quest actually in the bot's log
    std::string className;  // "warrior" ... "death knight"; never empty for a real bot
    std::string level;      // decimal, as text
    std::string zone;       // the bot's current zone name
    std::string guild;      // empty when the bot is unguilded
};

// Resolves every universal placeholder in `text` in place. Returns false if
// a placeholder in the text has no value in `ctx`, or if a card-only token
// is still standing afterwards (a card_gated line that reached an uncarded
// bot); the caller must discard `text` rather than deliver it in that case
// -- a line is either fully resolved or not delivered at all.
//
// The leftover check matches the two card-only tokens by name rather than
// any `%`-shaped substring, since ordinary chat can contain a literal `%`
// ("100% sure") and Hs_PlaceholderDiscipline already closes the set of
// tokens that can reach here.
//
// Pure and header-only, no AzerothCore dependency, so the standalone
// harness can cover it.
inline bool Hs_ResolveUniversalPlaceholders(std::string& text, const HsPlaceholderContext& ctx)
{
    struct Entry { const char* token; const std::string* value; };
    const Entry kEntries[] = {
        { "%item_link",  &ctx.itemLink  },
        { "%quest_link", &ctx.questLink },
        { "%class",      &ctx.className },
        { "%level",      &ctx.level     },
        { "%zone",       &ctx.zone      },
        { "%guild",      &ctx.guild     },
    };

    for (Entry const& entry : kEntries)
    {
        std::string token(entry.token);
        size_t pos = text.find(token);
        if (pos == std::string::npos)
            continue;
        if (entry.value->empty())
            return false; // nothing true to substitute -- drop the line

        while (pos != std::string::npos)
        {
            text.replace(pos, token.size(), *entry.value);
            pos = text.find(token, pos + entry.value->size());
        }
    }

    if (text.find("%main_focus") != std::string::npos || text.find("%current_goal") != std::string::npos)
        return false;

    return true;
}

// One tradeable item out of a bot's bags, for the grounded TradePrice
// answer (Claude/PLAN-TRADE.md). Deliberately primitives plus the ready-made
// link rather than an `Item*`/`ItemTemplate const*`, so this header stays
// free of the core's item headers the way it already stays free of the rest
// of AzerothCore.
//
// `count` is the stack size, which the price depends on: mod-playerbots'
// TradeStatusAction::CalculateCost charges per stack, not per unit, so a
// quote for a stack of 20 has to say 20.
struct HsTradeableItem
{
    uint32_t    itemId = 0;
    uint32_t    count  = 0;
    std::string link;   // the same |cff...|Hitem:...|h[...]|h|r markup %item_link produces
};

// Picks one non-soulbound item from the bot's backpack and equipped bags --
// the exact same selection %item_link uses (they share one implementation),
// so a WTS line and a price quote are drawn from the same pool and by the
// same rule. Soulbound is excluded because an item the bot cannot hand over
// is exactly the falsifiable claim §4.13 exists to prevent, and that applies
// at least as strongly to quoting a price for one.
//
// Returns false when the bot is carrying nothing tradeable, which the caller
// turns into the "not selling anything" answer rather than a fabricated
// price.
//
// Note this picks *a* tradeable item, not necessarily the one the bot last
// advertised -- the module does not record which item a past WTS line named.
// That is why the TradePrice reply interpolates the item link into the text:
// the bot says what it is quoting, so the answer is coherent on its own
// terms even when it is not the item the player had in mind.
bool Hs_PickTradeableItem(Player* bot, HsTradeableItem& out);

// Builds the context above from live Player* state. Declared here but
// defined in hs_corpus.cpp, the AzerothCore-dependent half; every caller
// (hs_handler.cpp's TryCorpusFallback, hs_opener.cpp's FireOpener) is
// already on the world thread, the only place a Player* may be touched.
//
// %item_link draws a random non-soulbound item from the bot's backpack and
// equipped bags -- soulbound is excluded since an item the bot can't hand
// over would be exactly the falsifiable claim §4.13 exists to prevent.
HsPlaceholderContext Hs_BuildPlaceholderContext(Player* bot);

// Scripted bot-to-bot dialogue's placeholder resolution (§4.16) --
// %my_class/%my_level/%my_zone/%my_guild bind to whichever bot is speaking
// this turn, %other_class/... to the other cast bot, resolved fresh per
// turn from live state since a script isn't tied to a specific pair of
// bots until two are actually cast together. Only the four personal-fact
// fields are used; `mine`/`other`'s itemLink/questLink are ignored, since
// scripts are casual small talk, not the trade content those tokens serve.
//
// Same "empty field drops the whole turn" contract as
// Hs_ResolveUniversalPlaceholders. Pure and header-only, no AzerothCore
// dependency, same reasoning as this file's other resolvers.
inline bool Hs_ResolveScriptPlaceholders(std::string& text, const HsPlaceholderContext& mine,
                                          const HsPlaceholderContext& other)
{
    struct Entry { const char* token; const std::string* value; };
    const Entry kEntries[] = {
        { "%my_class",     &mine.className  },
        { "%my_level",     &mine.level      },
        { "%my_zone",      &mine.zone       },
        { "%my_guild",     &mine.guild      },
        { "%other_class",  &other.className },
        { "%other_level",  &other.level     },
        { "%other_zone",   &other.zone      },
        { "%other_guild",  &other.guild     },
    };

    for (Entry const& entry : kEntries)
    {
        std::string token(entry.token);
        size_t pos = text.find(token);
        if (pos == std::string::npos)
            continue;
        if (entry.value->empty())
            return false; // nothing true to substitute -- drop the turn

        while (pos != std::string::npos)
        {
            text.replace(pos, token.size(), *entry.value);
            pos = text.find(token, pos + entry.value->size());
        }
    }

    return true;
}

// Class id -> the lowercase class name this module uses in prompts, card
// facts, and corpus text. Lives here rather than staying file-local in
// hs_generator.cpp so the generator's prompt labels and the delivery path's
// %class substitution can never drift apart.
std::string Hs_ClassNameFor(uint8_t classId);

#endif // MOD_HS_CORPUS_H
