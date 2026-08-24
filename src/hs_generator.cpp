#include "hs_generator.h"
#include "hs_archetype.h"
#include "hs_channel.h"
#include "hs_config.h"
#include "hs_corpus.h"
#include "hs_gen_validate.h"
#include "hs_identity.h"
#include "hs_identity_store.h"
#include "hs_json.h"
#include "hs_llm.h"
#include "hs_queue.h"

#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "Log.h"
#include "Random.h"
#include "SharedDefines.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    std::thread           g_GeneratorThread;
    std::atomic<bool>     g_StopGenerator{false};
    std::atomic<uint32_t> g_RowsAddedThisSession{0};
    std::atomic<uint32_t> g_RowsEvictedThisSession{0};

    // Compiled constants, not config keys -- there's no data yet to judge
    // the right turn count from, so this ships as a placeholder range.
    // Randomized per script (RunOneScriptGenerationCycle) rather than fixed,
    // so consecutive /say exchanges don't all read as the same fixed-length
    // shape.
    constexpr int kScriptTurnCountMin = 2;
    constexpr int kScriptTurnCountMax = 6;

    // §4.17: 2 turns, not 4 -- a 4-turn exchange scrolling through a
    // channel is conspicuous in a way the same script overheard in /say is
    // not (PLAN.md §4.17).
    constexpr int kChannelScriptTurnCount = 2;

    // Same "compiled constant, no data yet" shape as kScriptTurnCount --
    // three times hs_identity.h's kHsCardDormancyDays, since an unused
    // corpus line costs nothing while it waits, and eviction should catch
    // rows that are never picked, not ones that just haven't come up yet.
    constexpr uint32_t kHsGenUnusedRowEvictionDays = 90;

    // The 10 playable WotLK classes (id 10 is unused in the class enum).
    const std::vector<uint8_t> kValidClassIds = {
        CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST,
        CLASS_DEATH_KNIGHT, CLASS_SHAMAN, CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID,
    };

    // The four level_band_tag labels (hs_corpus.h's Hs_LevelBandFor maps a
    // *level* to one of these; here the generator just needs the fixed
    // label set to enumerate buckets, not a level->band lookup).
    const std::vector<std::string> kLevelBands = { "low", "mid", "high", "endgame" };

    // faction_tag's two values (0 = Alliance, 1 = Horde), matching
    // Player::GetTeamId()'s own convention -- same values hs_corpus.cpp's
    // TagWhereFor and every caller now thread through.
    const std::vector<std::pair<uint8_t, std::string>> kFactionIds = {
        { 0, "Alliance" }, { 1, "Horde" },
    };

    // A curated slice of zone ids, not every WotLK zone -- ids verified
    // against azerothcore-wotlk-pb/data/sql/base/db_world/graveyard_zone.sql
    // rather than guessed. Spans both factions' starting zones through a
    // handful of classic leveling hubs and Northrend; the generator grows
    // coverage from here over time rather than this list trying to be
    // exhaustive up front.
    const std::vector<std::pair<uint32_t, std::string>> kZoneIds = {
        { 12,   "Elwynn Forest" },
        { 1,    "Dun Morogh" },
        { 141,  "Teldrassil" },
        { 14,   "Durotar" },
        { 85,   "Tirisfal Glades" },
        { 215,  "Mulgore" },
        { 44,   "Redridge Mountains" },
        { 38,   "Loch Modan" },
        { 130,  "Silverpine Forest" },
        { 17,   "the Barrens" },
        { 33,   "Stranglethorn Vale" },
        { 15,   "Dustwallow Marsh" },
        { 3537, "Borean Tundra" },
        { 495,  "Howling Fjord" },
        { 65,   "Dragonblight" },
        { 210,  "Icecrown" },
    };

    struct HsGenBucket
    {
        std::string category;
        std::string tagColumn;     // "" | "class_tag" | "level_band_tag"
        std::string tagValueSql;   // "" | "2" | "'mid'" -- ready for a WHERE clause
        std::string tagValueLabel; // "" | "warrior" | "mid" -- for the prompt
        bool        cardGated;
    };

    // All rows in one exact bucket -- unlimited, since dedup has to check a
    // candidate against every existing row, not a sample.
    std::vector<std::string> AllRowsInBucket(const std::string& category, const std::string& tagColumn,
                                              const std::string& tagValueSql)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT text FROM hside_corpus WHERE name = '{}' {}",
            category, tagColumn.empty() ? "" : ("AND " + tagColumn + " = " + tagValueSql));
        std::vector<std::string> rows;
        if (!result)
            return rows;
        do { rows.push_back((*result)[0].Get<std::string>()); } while (result->NextRow());
        return rows;
    }

    // A small random sample -- only for the generation prompt's
    // diversity-forcing text, not for validation.
    std::vector<std::string> SampleRows(const std::string& category, const std::string& tagColumn,
                                         const std::string& tagValueSql, int limit)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT text FROM hside_corpus WHERE name = '{}' {} ORDER BY RAND() LIMIT {}",
            category, tagColumn.empty() ? "" : ("AND " + tagColumn + " = " + tagValueSql), limit);
        std::vector<std::string> rows;
        if (!result)
            return rows;
        do { rows.push_back((*result)[0].Get<std::string>()); } while (result->NextRow());
        return rows;
    }

    // Enumerates every (category, bucket) pair for every tag axis a seeded
    // category now uses (none/class/level_band/faction/zone), each with its
    // current row count.
    std::vector<std::pair<HsGenBucket, uint32_t>> EnumerateBucketsWithCounts()
    {
        std::vector<std::pair<HsGenBucket, uint32_t>> result;

        QueryResult catResult = CharacterDatabase.Query("SELECT name, tag_axis, card_gated FROM hside_corpus_category");
        if (!catResult)
            return result;

        do
        {
            std::string category  = (*catResult)[0].Get<std::string>();
            std::string axis      = (*catResult)[1].Get<std::string>();
            bool        cardGated = (*catResult)[2].Get<uint8_t>() != 0;

            if (axis == "none")
            {
                QueryResult countResult = CharacterDatabase.Query(
                    "SELECT COUNT(*) FROM hside_corpus WHERE name = '{}'", category);
                uint32_t count = countResult ? (*countResult)[0].Get<uint32_t>() : 0;
                result.push_back({ HsGenBucket{ category, "", "", "", cardGated }, count });
            }
            else if (axis == "class")
            {
                QueryResult countResult = CharacterDatabase.Query(
                    "SELECT class_tag, COUNT(*) FROM hside_corpus WHERE name = '{}' AND class_tag IS NOT NULL GROUP BY class_tag",
                    category);
                std::vector<std::pair<uint8_t, uint32_t>> counts;
                if (countResult)
                {
                    do { counts.emplace_back((*countResult)[0].Get<uint8_t>(), (*countResult)[1].Get<uint32_t>()); }
                    while (countResult->NextRow());
                }
                for (uint8_t classId : kValidClassIds)
                {
                    uint32_t count = 0;
                    for (auto const& c : counts) if (c.first == classId) { count = c.second; break; }
                    result.push_back({ HsGenBucket{ category, "class_tag", std::to_string(classId), Hs_ClassNameFor(classId), cardGated }, count });
                }
            }
            else if (axis == "level_band")
            {
                QueryResult countResult = CharacterDatabase.Query(
                    "SELECT level_band_tag, COUNT(*) FROM hside_corpus WHERE name = '{}' AND level_band_tag IS NOT NULL GROUP BY level_band_tag",
                    category);
                std::vector<std::pair<std::string, uint32_t>> counts;
                if (countResult)
                {
                    do { counts.emplace_back((*countResult)[0].Get<std::string>(), (*countResult)[1].Get<uint32_t>()); }
                    while (countResult->NextRow());
                }
                for (auto const& band : kLevelBands)
                {
                    uint32_t count = 0;
                    for (auto const& c : counts) if (c.first == band) { count = c.second; break; }
                    result.push_back({ HsGenBucket{ category, "level_band_tag", "'" + band + "'", band, cardGated }, count });
                }
            }
            else if (axis == "faction")
            {
                QueryResult countResult = CharacterDatabase.Query(
                    "SELECT faction_tag, COUNT(*) FROM hside_corpus WHERE name = '{}' AND faction_tag IS NOT NULL GROUP BY faction_tag",
                    category);
                std::vector<std::pair<uint8_t, uint32_t>> counts;
                if (countResult)
                {
                    do { counts.emplace_back((*countResult)[0].Get<uint8_t>(), (*countResult)[1].Get<uint32_t>()); }
                    while (countResult->NextRow());
                }
                for (auto const& faction : kFactionIds)
                {
                    uint32_t count = 0;
                    for (auto const& c : counts) if (c.first == faction.first) { count = c.second; break; }
                    result.push_back({ HsGenBucket{ category, "faction_tag", std::to_string(faction.first), faction.second, cardGated }, count });
                }
            }
            else if (axis == "zone")
            {
                QueryResult countResult = CharacterDatabase.Query(
                    "SELECT zone_tag, COUNT(*) FROM hside_corpus WHERE name = '{}' AND zone_tag IS NOT NULL GROUP BY zone_tag",
                    category);
                std::vector<std::pair<uint32_t, uint32_t>> counts;
                if (countResult)
                {
                    do { counts.emplace_back((*countResult)[0].Get<uint32_t>(), (*countResult)[1].Get<uint32_t>()); }
                    while (countResult->NextRow());
                }
                for (auto const& zone : kZoneIds)
                {
                    uint32_t count = 0;
                    for (auto const& c : counts) if (c.first == zone.first) { count = c.second; break; }
                    result.push_back({ HsGenBucket{ category, "zone_tag", std::to_string(zone.first), zone.second, cardGated }, count });
                }
            }
        } while (catResult->NextRow());

        return result;
    }

    std::string BuildGenerationPrompt(const HsGenBucket& bucket, const std::vector<std::string>& promptSampleRows,
                                       bool sampleIsSiblingFallback, bool requiresPlaceholder)
    {
        std::string prompt =
            "You are helping write ambient background chat lines for an ordinary World of "
            "Warcraft: Wrath of the Lich King player -- the way real players actually type "
            "in /say or general chat, not narration or descriptive prose. Write exactly ONE "
            "short, casual, grammatically clean sentence: a concrete opinion, gripe, or "
            "observation about actual gameplay (a quest, a fight, gear, a class/spec choice, "
            "grouping, professions, travel time) -- not a question, not addressed to anyone, "
            "first person. Never describe scenery for its own sake (no 'the way the "
            "light...', no calling something peaceful/breathtaking/beautiful) -- if a place "
            "comes up, talk about what's actually happening there for a player, not what it "
            "looks like. Never compare to how things usually are or used to be ('more than "
            "usual', 'lately', 'these days', 'still') -- this line is written once and reused "
            "for any player at any time, so it can't reference a real trend. No markdown, no "
            "emoji, no quotation marks around the line itself, no modern internet slang.";

        if (!bucket.tagValueLabel.empty())
        {
            if (bucket.tagColumn == "zone_tag")
                prompt += " Write this one as something a player currently in " + bucket.tagValueLabel +
                          " would say about being there -- not a description of the zone itself.";
            else if (bucket.tagColumn == "level_band_tag")
                prompt += " Write this one as something a player around the " + bucket.tagValueLabel +
                          " level range would say.";
            else
                prompt += " Write this one as something a " + bucket.tagValueLabel + " player specifically would say.";
        }

        if (requiresPlaceholder)
            prompt += " This category's lines always include a game placeholder token like "
                      "%item_link written literally -- yours must include one too.";

        if (!promptSampleRows.empty())
        {
            prompt += sampleIsSiblingFallback
                ? " Existing lines from a related bucket in this category (for tone only -- "
                  "yours is for a different case, write something new, not a variation of these):"
                : " Existing lines already in this exact category (for tone and topic reference "
                  "only -- write something different, not a variation of these):";
            for (auto const& row : promptSampleRows)
                prompt += "\n- " + row;
        }

        return prompt;
    }

    // One full attempt: pick an under-quota bucket, generate one candidate,
    // validate, insert on accept. Returns true only if a row was actually
    // added -- the caller uses that to decide how eagerly to loop back.
    bool RunOneGenerationCycle()
    {
        std::vector<std::pair<HsGenBucket, uint32_t>> buckets = EnumerateBucketsWithCounts();
        std::vector<HsGenBucket> underQuota;
        for (auto const& entry : buckets)
            if (entry.second < g_HsGeneratorRowsPerBucket)
                underQuota.push_back(entry.first);

        if (underQuota.empty())
            return false; // quota satisfied everywhere -- caller backs off

        HsGenBucket const& bucket = underQuota[urand(0, static_cast<uint32_t>(underQuota.size() - 1))];

        std::vector<std::string> bucketRows = AllRowsInBucket(bucket.category, bucket.tagColumn, bucket.tagValueSql);
        bool usesSiblingSample = bucketRows.empty();
        std::vector<std::string> promptSampleRows = usesSiblingSample
            ? SampleRows(bucket.category, "", "", 5) // any tag value in this category, tone reference only
            : SampleRows(bucket.category, bucket.tagColumn, bucket.tagValueSql, 5);

        bool requiresPlaceholder = false;
        for (auto const& row : bucketRows)
            if (Hs_ContainsPlaceholder(row)) { requiresPlaceholder = true; break; }

        std::string prompt = BuildGenerationPrompt(bucket, promptSampleRows, usesSiblingSample, requiresPlaceholder);

        HsLLMConfig cfg;
        cfg.apiType       = g_HsGeneratorLLMApiType;
        cfg.baseUrl       = g_HsGeneratorLLMUrl;
        cfg.model         = g_HsGeneratorLLMModel;
        cfg.apiKey        = g_HsGeneratorLLMApiKey;
        cfg.timeoutSec    = static_cast<int>(g_HsGeneratorLLMTimeoutSeconds);
        cfg.maxTokens     = static_cast<int>(g_HsGeneratorLLMMaxTokens);
        cfg.dryMultiplier = 0.0f;

        HsLLMResult result = Hs_CallLLM(cfg, prompt, "", {}, "Write one new line now. Reply with only the line itself.");
        if (!result.success || result.text.empty())
        {
            if (g_HsDebugEnabled)
                LOG_INFO("server.loading", "[HearthsideChat] Generator: LLM call failed for bucket {}/{} (failure={}).",
                    bucket.category, bucket.tagValueLabel, static_cast<int>(result.failure));
            return false;
        }

        HsGenVerdict verdict = Hs_TryInsertCorpusRow(bucket.category, bucket.tagColumn, bucket.tagValueSql,
                                                      result.text, g_HsGeneratorLLMModel, g_HsGeneratorPromptVersion);
        if (g_HsDebugEnabled)
            LOG_INFO("server.loading", "[HearthsideChat] Generator: bucket {}/{} candidate {} -- \"{}\"",
                bucket.category, bucket.tagValueLabel,
                verdict.accepted ? "accepted" : ("rejected (" + verdict.reason + ")"), result.text);

        if (verdict.accepted)
            g_RowsAddedThisSession.fetch_add(1);

        return verdict.accepted;
    }

    uint32_t ScriptReserveDepthQuery()
    {
        // channel IS NULL: a §4.17 channel script also has consumed_at IS
        // NULL while unclaimed, but belongs to its own reserve
        // (ChannelScriptReserveDepthQuery below), not this /say count.
        QueryResult result = CharacterDatabase.Query("SELECT COUNT(*) FROM hside_script WHERE consumed_at IS NULL AND channel IS NULL");
        return result ? (*result)[0].Get<uint32_t>() : 0;
    }

    uint32_t ChannelScriptReserveDepthQuery(HsChannelKind kind)
    {
        std::string channelColumn = std::string(Hs_ChannelKindName(kind));
        std::transform(channelColumn.begin(), channelColumn.end(), channelColumn.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        QueryResult result = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM hside_script WHERE consumed_at IS NULL AND channel = '{}'", channelColumn);
        return result ? (*result)[0].Get<uint32_t>() : 0;
    }

    // Baseline persona only, no archetype and no card -- personality is
    // applied per speaker at delivery by the style pass (hs_script.cpp), so
    // the generator's job is clean, neutral dialogue: gripes, opinions, and
    // preferences, nothing checkable. A live-fire test still produced one
    // script naming a real dungeon under an earlier version with no escape
    // hatch, so this version gives the model a safe, closed vocabulary for
    // the one class of personal fact players actually mention in small talk
    // (own/other's class, level, zone, guild): the eight %my_*/%other_*
    // tokens, resolved per bot at delivery time (hs_corpus.h's
    // Hs_ResolveScriptPlaceholders) so a claim is only ever true of
    // whichever two bots end up cast. A specific item, quest, or invented
    // biography still has no placeholder and stays flatly disallowed.
    const std::string kScriptSystemPrompt =
        "You are an ordinary player in World of Warcraft: Wrath of the Lich King, making small "
        "talk with another player you don't know well. Keep it casual, brief, one short line at "
        "a time -- the way real players actually chat. Stick to general opinions, feelings, and "
        "gripes about the game. If you want to mention your own class, level, current zone, or "
        "guild, write exactly one of these tokens instead of naming one directly: %my_class, "
        "%my_level, %my_zone, %my_guild. For the other player's, use: %other_class, "
        "%other_level, %other_zone, %other_guild. Never invent or state a specific item, quest, "
        "or any other detail the other person could check and find wrong. No roleplay "
        "narration, no asterisks, no mention of being an AI or a game.";

    const std::string kScriptOpeningTrigger =
        "(You've just noticed another player standing nearby. Say something casual to strike up "
        "a short conversation.)";

    // One full attempt: generate a randomized-length (kScriptTurnCountMin..
    // kScriptTurnCountMax) run of lines as a single continuous exchange
    // (reusing the reactive tier's own history-append mechanism, hs_llm.h's
    // HsHistoryTurn -- turn N's trigger is turn N-1's text, so the model is
    // always just replying, the same shape as two real people alternating).
    // Turns are labelled speaker_slot 0/1 by position after the fact; the
    // model never needs to know there are two characters, since both share
    // the identical baseline voice. A single line per call is required, not
    // a stylistic choice -- Hs_CallLLM stops generation at the first newline
    // (hs_llm.cpp), so one call cannot produce a multi-turn script.
    //
    // Aborts (returns false, no partial script ever inserted) on the first
    // LLM failure or quality-gate rejection; the caller's backoff already
    // handles retrying later, the same as a failed corpus generation.
    bool RunOneScriptGenerationCycle()
    {
        HsLLMConfig cfg;
        cfg.apiType       = g_HsGeneratorLLMApiType;
        cfg.baseUrl       = g_HsGeneratorLLMUrl;
        cfg.model         = g_HsGeneratorLLMModel;
        cfg.apiKey        = g_HsGeneratorLLMApiKey;
        cfg.timeoutSec    = static_cast<int>(g_HsGeneratorLLMTimeoutSeconds);
        cfg.maxTokens     = static_cast<int>(g_HsGeneratorLLMMaxTokens);
        cfg.dryMultiplier = 0.0f;

        int turnCount = static_cast<int>(urand(kScriptTurnCountMin, kScriptTurnCountMax));

        std::vector<HsHistoryTurn> history;
        std::string prevText = kScriptOpeningTrigger;
        std::vector<std::pair<uint8_t, std::string>> turns; // slot, text

        for (int i = 0; i < turnCount; ++i)
        {
            HsLLMResult result = Hs_CallLLM(cfg, kScriptSystemPrompt, "", history, prevText);
            if (!result.success || result.text.empty())
            {
                if (g_HsDebugEnabled)
                    LOG_INFO("server.loading", "[HearthsideChat] Generator: script turn {} LLM call failed (failure={}).",
                        i, static_cast<int>(result.failure));
                return false;
            }

            HsGenVerdict verdict = Hs_QualityGate(result.text, /*allowQuestions=*/true);
            if (verdict.accepted)
                verdict = Hs_ScriptPlaceholderDiscipline(result.text);
            if (!verdict.accepted)
            {
                if (g_HsDebugEnabled)
                    LOG_INFO("server.loading", "[HearthsideChat] Generator: script turn {} rejected ({}) -- \"{}\"",
                        i, verdict.reason, result.text);
                return false;
            }

            turns.emplace_back(static_cast<uint8_t>(i % 2), result.text);
            history.push_back({ prevText, result.text });
            prevText = result.text;
        }

        // Inserting a row with AUTO_INCREMENT and then reading back its new
        // id is unsafe over CharacterDatabase's pooled synchronous
        // connections (LAST_INSERT_ID() is session-scoped and the pool does
        // not guarantee two calls share a connection). Only this generator
        // thread ever writes to hside_script, so MAX(id)+1 has no concurrent
        // writer to race against -- application-side id generation, the
        // same idiom ObjectMgr uses for mail/auction ids, without needing a
        // persistent in-memory counter for something this infrequent.
        QueryResult idResult = CharacterDatabase.Query("SELECT COALESCE(MAX(id), 0) + 1 FROM hside_script");
        uint32_t scriptId = idResult ? (*idResult)[0].Get<uint32_t>() : 1;

        std::string escapedModel = g_HsGeneratorLLMModel;
        CharacterDatabase.EscapeString(escapedModel);
        std::string escapedVersion = g_HsGeneratorPromptVersion;
        CharacterDatabase.EscapeString(escapedVersion);
        std::string modelSql   = escapedModel.empty()   ? "NULL" : ("'" + escapedModel + "'");
        std::string versionSql = escapedVersion.empty() ? "NULL" : ("'" + escapedVersion + "'");

        CharacterDatabase.Execute(
            "INSERT INTO hside_script (id, turn_count, generated_at, model, prompt_version) VALUES ({}, {}, NOW(), {}, {})",
            scriptId, turnCount, modelSql, versionSql);

        for (size_t i = 0; i < turns.size(); ++i)
        {
            std::string escapedText = turns[i].second;
            CharacterDatabase.EscapeString(escapedText);
            CharacterDatabase.Execute(
                "INSERT INTO hside_script_turn (script_id, turn_no, speaker_slot, text) VALUES ({}, {}, {}, '{}')",
                scriptId, static_cast<uint32_t>(i), turns[i].first, escapedText);
        }

        if (g_HsDebugEnabled)
            LOG_INFO("server.loading", "[HearthsideChat] Generator: script {} inserted ({} turns).", scriptId, turns.size());

        g_RowsAddedThisSession.fetch_add(1);
        return true;
    }

    // §4.17: a channel variant of the exchange above -- shorter (2 turns),
    // and swaps the neutral small-talk framing for one naming the channel
    // itself, so the model's opening line reads as belonging there instead
    // of a chance meeting. Trade still forbids AH-shaped price claims (same
    // §4.13 rule TRADER's live-price commentary follows) since a generated
    // line has no real item/price behind it the way a grounded lookup would.
    // Same turn-by-turn call/quality-gate/placeholder-discipline shape as
    // RunOneScriptGenerationCycle, just parameterized by kind and turn count.
    std::string ChannelScriptSystemPromptFor(HsChannelKind kind)
    {
        switch (kind)
        {
            case HsChannelKind::Trade:
                return "You are an ordinary player in World of Warcraft: Wrath of the Lich King, "
                       "chatting in the Trade channel with another player you don't know well. Keep "
                       "it casual and brief, one short line at a time. Talk about gearing up, "
                       "professions, or gripes about prices in general terms -- never a specific "
                       "item, quest, or exact gold price the other person could check and find "
                       "wrong. If you want to mention your own class, level, current zone, or "
                       "guild, write exactly one of these tokens instead of naming one directly: "
                       "%my_class, %my_level, %my_zone, %my_guild. For the other player's, use: "
                       "%other_class, %other_level, %other_zone, %other_guild. No roleplay "
                       "narration, no asterisks, no mention of being an AI or a game.";
            case HsChannelKind::General:
                return "You are an ordinary player in World of Warcraft: Wrath of the Lich King, "
                       "chatting in the General channel with another player you don't know well. "
                       "Keep it casual and brief, one short line at a time -- zone flavor, quests, "
                       "or general opinions about the game. If you want to mention your own class, "
                       "level, current zone, or guild, write exactly one of these tokens instead of "
                       "naming one directly: %my_class, %my_level, %my_zone, %my_guild. For the "
                       "other player's, use: %other_class, %other_level, %other_zone, %other_guild. "
                       "Never invent or state a specific item, quest, or any other detail the other "
                       "person could check and find wrong. No roleplay narration, no asterisks, no "
                       "mention of being an AI or a game.";
            case HsChannelKind::World:
            default:
                return "You are an ordinary player in World of Warcraft: Wrath of the Lich King, "
                       "chatting in the realm-wide World channel with another player you don't know "
                       "well. Keep it casual and brief, one short line at a time -- general opinions "
                       "or banter about the game, nothing tied to one zone. If you want to mention "
                       "your own class, level, current zone, or guild, write exactly one of these "
                       "tokens instead of naming one directly: %my_class, %my_level, %my_zone, "
                       "%my_guild. For the other player's, use: %other_class, %other_level, "
                       "%other_zone, %other_guild. Never invent or state a specific item, quest, or "
                       "any other detail the other person could check and find wrong. No roleplay "
                       "narration, no asterisks, no mention of being an AI or a game.";
        }
    }

    bool RunOneChannelScriptGenerationCycle(HsChannelKind kind)
    {
        HsLLMConfig cfg;
        cfg.apiType       = g_HsGeneratorLLMApiType;
        cfg.baseUrl       = g_HsGeneratorLLMUrl;
        cfg.model         = g_HsGeneratorLLMModel;
        cfg.apiKey        = g_HsGeneratorLLMApiKey;
        cfg.timeoutSec    = static_cast<int>(g_HsGeneratorLLMTimeoutSeconds);
        cfg.maxTokens     = static_cast<int>(g_HsGeneratorLLMMaxTokens);
        cfg.dryMultiplier = 0.0f;

        std::string systemPrompt = ChannelScriptSystemPromptFor(kind);
        std::vector<HsHistoryTurn> history;
        std::string prevText = kScriptOpeningTrigger;
        std::vector<std::pair<uint8_t, std::string>> turns;

        for (int i = 0; i < kChannelScriptTurnCount; ++i)
        {
            HsLLMResult result = Hs_CallLLM(cfg, systemPrompt, "", history, prevText);
            if (!result.success || result.text.empty())
            {
                if (g_HsDebugEnabled)
                    LOG_INFO("server.loading", "[HearthsideChat] Generator: channel script turn {} LLM call failed (failure={}).",
                        i, static_cast<int>(result.failure));
                return false;
            }

            HsGenVerdict verdict = Hs_QualityGate(result.text, /*allowQuestions=*/true);
            if (verdict.accepted)
                verdict = Hs_ScriptPlaceholderDiscipline(result.text);
            if (!verdict.accepted)
            {
                if (g_HsDebugEnabled)
                    LOG_INFO("server.loading", "[HearthsideChat] Generator: channel script turn {} rejected ({}) -- \"{}\"",
                        i, verdict.reason, result.text);
                return false;
            }

            turns.emplace_back(static_cast<uint8_t>(i % 2), result.text);
            history.push_back({ prevText, result.text });
            prevText = result.text;
        }

        QueryResult idResult = CharacterDatabase.Query("SELECT COALESCE(MAX(id), 0) + 1 FROM hside_script");
        uint32_t scriptId = idResult ? (*idResult)[0].Get<uint32_t>() : 1;

        std::string escapedModel = g_HsGeneratorLLMModel;
        CharacterDatabase.EscapeString(escapedModel);
        std::string escapedVersion = g_HsGeneratorPromptVersion;
        CharacterDatabase.EscapeString(escapedVersion);
        std::string modelSql   = escapedModel.empty()   ? "NULL" : ("'" + escapedModel + "'");
        std::string versionSql = escapedVersion.empty() ? "NULL" : ("'" + escapedVersion + "'");

        std::string channelColumn = std::string(Hs_ChannelKindName(kind));
        std::transform(channelColumn.begin(), channelColumn.end(), channelColumn.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        CharacterDatabase.Execute(
            "INSERT INTO hside_script (id, turn_count, channel, generated_at, model, prompt_version) VALUES ({}, {}, '{}', NOW(), {}, {})",
            scriptId, kChannelScriptTurnCount, channelColumn, modelSql, versionSql);

        for (size_t i = 0; i < turns.size(); ++i)
        {
            std::string escapedText = turns[i].second;
            CharacterDatabase.EscapeString(escapedText);
            CharacterDatabase.Execute(
                "INSERT INTO hside_script_turn (script_id, turn_no, speaker_slot, text) VALUES ({}, {}, {}, '{}')",
                scriptId, static_cast<uint32_t>(i), turns[i].first, escapedText);
        }

        if (g_HsDebugEnabled)
            LOG_INFO("server.loading", "[HearthsideChat] Generator: channel script {} inserted for {} ({} turns).",
                scriptId, channelColumn, turns.size());

        g_RowsAddedThisSession.fetch_add(1);
        return true;
    }

    uint32_t PendingCardCount()
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM hside_identity WHERE promoted_at IS NOT NULL AND card_voice IS NULL");
        return result ? (*result)[0].Get<uint32_t>() : 0;
    }

    // One promoted-but-uncarded bot's row (archetype is recomputed from
    // Hs_ArchetypeForBot(guid, lastKnownLevel) rather than parsed back from
    // the stored name column -- that function is pure and already the
    // source of truth hs_queue.cpp's WorkerLoop uses, so this avoids
    // needing a second string->enum lookup that would exist only for this
    // one call site).
    struct PendingCard
    {
        uint64_t botGuid;
        uint8_t  lastKnownLevel;
    };

    bool ClaimOnePendingCard(PendingCard& out)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT bot_guid, last_known_level FROM hside_identity "
            "WHERE promoted_at IS NOT NULL AND card_voice IS NULL LIMIT 1");
        if (!result)
            return false;
        out.botGuid        = (*result)[0].Get<uint64_t>();
        out.lastKnownLevel = (*result)[1].Get<uint8_t>();
        return true;
    }

    // hasGuild/guildName straight from the characters/guild_member/guild
    // tables -- the generator thread never touches Player*, resolving it in
    // SQL instead, the same shape the rest of this file already uses for
    // class/level bucket enumeration.
    struct GuildLookup
    {
        bool        hasGuild = false;
        std::string name;
    };

    GuildLookup LookupGuildFor(uint64_t botGuid)
    {
        GuildLookup lookup;
        QueryResult result = CharacterDatabase.Query(
            "SELECT g.name FROM characters c "
            "JOIN guild_member gm ON gm.guid = c.guid "
            "JOIN guild g ON g.guildid = gm.guildid "
            "WHERE c.guid = {}", botGuid);
        if (result)
        {
            lookup.hasGuild = true;
            lookup.name     = (*result)[0].Get<std::string>();
        }
        return lookup;
    }

    // One card, both halves (a voice block and a fact sheet), two LLM
    // calls. Aborts (returns false, no partial card ever written) on the
    // first LLM failure or validation rejection, retrying next cycle the
    // same as script generation. On success: writes both halves, flips
    // card_active, and pushes the bot's name into playerbots' recycling-
    // exclusion vectors immediately rather than waiting for the next
    // startup/reload reconcile.
    bool RunOneCardGenerationCycle()
    {
        PendingCard pending;
        if (!ClaimOnePendingCard(pending))
            return false;

        HsArchetype archetype = Hs_ArchetypeForBot(pending.botGuid, pending.lastKnownLevel);
        const HsArchetypeInfo& archetypeInfo = Hs_ArchetypeInfoFor(archetype);
        GuildLookup guild = LookupGuildFor(pending.botGuid);

        HsLLMConfig cfg;
        cfg.apiType       = g_HsGeneratorLLMApiType;
        cfg.baseUrl       = g_HsGeneratorLLMUrl;
        cfg.model         = g_HsGeneratorLLMModel;
        cfg.apiKey        = g_HsGeneratorLLMApiKey;
        cfg.timeoutSec    = static_cast<int>(g_HsGeneratorLLMTimeoutSeconds);
        cfg.maxTokens     = static_cast<int>(g_HsGeneratorLLMMaxTokens);
        cfg.dryMultiplier = 0.0f;

        std::string voicePrompt = Hs_BuildVoiceBlockPrompt(archetypeInfo.talksAbout);
        HsLLMResult voiceResult = Hs_CallLLM(cfg, voicePrompt, "", {},
            "Write it now. Reply with only the persona note itself.");
        if (!voiceResult.success || voiceResult.text.empty())
        {
            if (g_HsDebugEnabled)
                LOG_INFO("server.loading", "[HearthsideChat] Generator: card voice-block call failed for bot {} (failure={}).",
                    pending.botGuid, static_cast<int>(voiceResult.failure));
            return false;
        }
        HsGenVerdict voiceVerdict = Hs_ValidateVoiceBlock(voiceResult.text);
        if (!voiceVerdict.accepted)
        {
            if (g_HsDebugEnabled)
                LOG_INFO("server.loading", "[HearthsideChat] Generator: card voice-block rejected for bot {} ({}) -- \"{}\"",
                    pending.botGuid, voiceVerdict.reason, voiceResult.text);
            return false;
        }

        std::string factsPrompt = Hs_BuildCardFactsPrompt(archetypeInfo.talksAbout, pending.lastKnownLevel,
                                                            guild.hasGuild, guild.name);
        HsLLMResult factsResult = Hs_CallLLM(cfg, factsPrompt, "", {},
            "Reply now with only the JSON, all on one line, no line breaks.");
        if (!factsResult.success || factsResult.text.empty())
        {
            if (g_HsDebugEnabled)
                LOG_INFO("server.loading", "[HearthsideChat] Generator: card facts call failed for bot {} (failure={}).",
                    pending.botGuid, static_cast<int>(factsResult.failure));
            return false;
        }
        hs_json facts = hs_json::parse(factsResult.text, nullptr, /*allow_exceptions=*/false);
        HsGenVerdict factsVerdict = facts.is_discarded()
            ? HsGenVerdict{ false, "not_valid_json" }
            : Hs_ValidateCardFacts(facts, pending.lastKnownLevel, guild.hasGuild);
        if (!factsVerdict.accepted)
        {
            if (g_HsDebugEnabled)
                LOG_INFO("server.loading", "[HearthsideChat] Generator: card facts rejected for bot {} ({}) -- \"{}\"",
                    pending.botGuid, factsVerdict.reason, factsResult.text);
            return false;
        }

        std::string escapedVoice = voiceResult.text;
        CharacterDatabase.EscapeString(escapedVoice);
        std::string factsCompact = facts.dump();
        CharacterDatabase.EscapeString(factsCompact);
        std::string escapedModel = g_HsGeneratorLLMModel;
        CharacterDatabase.EscapeString(escapedModel);
        std::string escapedVersion = g_HsGeneratorPromptVersion;
        CharacterDatabase.EscapeString(escapedVersion);
        std::string modelSql   = escapedModel.empty()   ? "NULL" : ("'" + escapedModel + "'");
        std::string versionSql = escapedVersion.empty() ? "NULL" : ("'" + escapedVersion + "'");

        // archetype is written back here too -- it's recomputed above from
        // (guid, lastKnownLevel) rather than trusted from the stored column,
        // same as hs_archetype.h's Hs_ArchetypeForBot always recomputing for
        // the level passed in. Without this the stored column can go stale
        // relative to the card actually generated if the bot's level
        // changed between promotion and this cycle running.
        CharacterDatabase.Execute(
            "UPDATE hside_identity SET archetype = '{}', card_voice = '{}', card_facts = '{}', "
            "card_model = {}, card_prompt_version = {}, card_active = 1 WHERE bot_guid = {}",
            archetypeInfo.enumName, escapedVoice, factsCompact, modelSql, versionSql, pending.botGuid);

        Hs_PushBotIntoExcludeVectors(pending.botGuid);

        if (g_HsDebugEnabled)
            LOG_INFO("server.loading", "[HearthsideChat] Generator: card for bot {} generated and activated.", pending.botGuid);

        g_RowsAddedThisSession.fetch_add(1);
        return true;
    }

    void GeneratorLoop()
    {
        while (!g_StopGenerator.load())
        {
            if (!g_HsGeneratorEnabled || !Hs_IsReactiveIdle())
            {
                std::this_thread::sleep_for(std::chrono::seconds(g_HsGeneratorPollIntervalSeconds));
                continue;
            }

            // Priority order: cards first, then the /say script reserve,
            // then the three channel-script reserves (§4.17 -- Trade,
            // General, World, in that order), then corpus buckets. Channel
            // reserves reuse the same g_HsGeneratorScriptReserveTarget as
            // the /say reserve rather than a separate per-channel config
            // key (Claude/ISSUES.md's "separate generator reserve or a
            // truncation rule" question, answered as "shared target" for
            // now -- easy to split later against live-realm evidence).
            bool added;
            if (PendingCardCount() > 0)
                added = RunOneCardGenerationCycle();
            else if (ScriptReserveDepthQuery() < g_HsGeneratorScriptReserveTarget)
                added = RunOneScriptGenerationCycle();
            else if (ChannelScriptReserveDepthQuery(HsChannelKind::Trade) < g_HsGeneratorScriptReserveTarget)
                added = RunOneChannelScriptGenerationCycle(HsChannelKind::Trade);
            else if (ChannelScriptReserveDepthQuery(HsChannelKind::General) < g_HsGeneratorScriptReserveTarget)
                added = RunOneChannelScriptGenerationCycle(HsChannelKind::General);
            else if (ChannelScriptReserveDepthQuery(HsChannelKind::World) < g_HsGeneratorScriptReserveTarget)
                added = RunOneChannelScriptGenerationCycle(HsChannelKind::World);
            else
                added = RunOneGenerationCycle();

            if (!added)
                std::this_thread::sleep_for(std::chrono::seconds(g_HsGeneratorQuotaSatisfiedBackoffSeconds));
            // else loop straight back -- re-checks idle before the next attempt.
        }
    }
}

void Hs_GeneratorStartup()
{
    g_StopGenerator.store(false);
    if (!g_GeneratorThread.joinable())
        g_GeneratorThread = std::thread(GeneratorLoop);
}

void Hs_GeneratorShutdown()
{
    g_StopGenerator.store(true);
    if (g_GeneratorThread.joinable())
        g_GeneratorThread.join();
}

uint32_t Hs_GeneratorRowsAddedThisSession()
{
    return g_RowsAddedThisSession.load();
}

uint32_t Hs_RowsEvictedThisSession()
{
    return g_RowsEvictedThisSession.load();
}

uint32_t Hs_ScriptReserveDepth()
{
    return ScriptReserveDepthQuery();
}

uint32_t Hs_RunEvictionSweep()
{
    uint32_t evictedTotal = 0;

    for (auto const& entry : EnumerateBucketsWithCounts())
    {
        HsGenBucket const& bucket = entry.first;
        uint32_t            count = entry.second;
        if (count <= g_HsGeneratorRowsPerBucket)
            continue;

        uint32_t overflow = count - g_HsGeneratorRowsPerBucket;
        std::string tagWhere = bucket.tagColumn.empty() ? "" : ("AND " + bucket.tagColumn + " = " + bucket.tagValueSql);

        // Exposure first (most-heard evicted first), generated_at second --
        // NULL (hand-authored) rows sort last among ties, so a tie between a
        // hand-authored line and a generator line evicts the generator one.
        CharacterDatabase.Execute(
            "DELETE FROM hside_corpus WHERE name = '{}' {} "
            "ORDER BY times_used DESC, (generated_at IS NULL) ASC, generated_at ASC LIMIT {}",
            bucket.category, tagWhere, overflow);
        evictedTotal += overflow;
        g_RowsEvictedThisSession.fetch_add(overflow);

        if (g_HsDebugEnabled)
            LOG_INFO("server.loading", "[HearthsideChat] Eviction: bucket {}/{} trimmed {} row(s) ({} -> {}).",
                bucket.category, bucket.tagValueLabel, overflow, count, g_HsGeneratorRowsPerBucket);
    }

    return evictedTotal;
}

uint32_t Hs_RunUnusedRowEvictionSweep()
{
    // COALESCE to generated_at: a row never selected has NULL last_used_at
    // (the column is only ever set on actual selection), so its own
    // creation time is the right reference point for "unused for months".
    QueryResult countResult = CharacterDatabase.Query(
        "SELECT COUNT(*) FROM hside_corpus WHERE generated_at IS NOT NULL "
        "AND COALESCE(last_used_at, generated_at) < NOW() - INTERVAL {} DAY",
        kHsGenUnusedRowEvictionDays);
    uint32_t count = countResult ? (*countResult)[0].Get<uint32_t>() : 0;
    if (count == 0)
        return 0;

    CharacterDatabase.Execute(
        "DELETE FROM hside_corpus WHERE generated_at IS NOT NULL "
        "AND COALESCE(last_used_at, generated_at) < NOW() - INTERVAL {} DAY",
        kHsGenUnusedRowEvictionDays);
    g_RowsEvictedThisSession.fetch_add(count);

    if (g_HsDebugEnabled)
        LOG_INFO("server.loading",
            "[HearthsideChat] Eviction: unused-row sweep removed {} row(s) unpicked for {}+ days.",
            count, kHsGenUnusedRowEvictionDays);

    return count;
}

uint32_t Hs_EvictGenerationRun(const std::string& promptVersion)
{
    std::string escaped = promptVersion;
    CharacterDatabase.EscapeString(escaped);

    QueryResult countResult = CharacterDatabase.Query(
        "SELECT COUNT(*) FROM hside_corpus WHERE prompt_version = '{}'", escaped);
    uint32_t count = countResult ? (*countResult)[0].Get<uint32_t>() : 0;
    if (count == 0)
        return 0;

    CharacterDatabase.Execute("DELETE FROM hside_corpus WHERE prompt_version = '{}'", escaped);
    g_RowsEvictedThisSession.fetch_add(count);
    return count;
}

std::vector<HsCorpusReviewRow> Hs_ReviewCorpusRows(const std::string& category, const std::string& promptVersion, uint32_t limit)
{
    std::vector<HsCorpusReviewRow> rows;

    std::string escapedCategory = category;
    CharacterDatabase.EscapeString(escapedCategory);
    std::string versionWhere;
    if (!promptVersion.empty())
    {
        std::string escapedVersion = promptVersion;
        CharacterDatabase.EscapeString(escapedVersion);
        versionWhere = "AND prompt_version = '" + escapedVersion + "'";
    }

    QueryResult result = CharacterDatabase.Query(
        "SELECT text, times_used, model, prompt_version FROM hside_corpus WHERE name = '{}' {} "
        "ORDER BY generated_at IS NULL, generated_at DESC, id DESC LIMIT {}",
        escapedCategory, versionWhere, limit);
    if (!result)
        return rows;

    do
    {
        HsCorpusReviewRow row;
        row.text          = (*result)[0].Get<std::string>();
        row.timesUsed      = (*result)[1].Get<uint32_t>();
        row.model          = (*result)[2].IsNull() ? "" : (*result)[2].Get<std::string>();
        row.promptVersion = (*result)[3].IsNull() ? "" : (*result)[3].Get<std::string>();
        rows.push_back(std::move(row));
    } while (result->NextRow());

    return rows;
}

std::string Hs_LookupCategoryAxis(const std::string& category)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT tag_axis FROM hside_corpus_category WHERE name = '{}'", category);
    return result ? (*result)[0].Get<std::string>() : "";
}

HsGenVerdict Hs_TryInsertCorpusRow(const std::string& category, const std::string& tagColumn,
                                    const std::string& tagValueSql, const std::string& candidateText,
                                    const std::string& model, const std::string& promptVersion)
{
    QueryResult catResult = CharacterDatabase.Query(
        "SELECT card_gated FROM hside_corpus_category WHERE name = '{}'", category);
    if (!catResult)
        return { false, "unknown_category" };
    bool cardGated = (*catResult)[0].Get<uint8_t>() != 0;

    std::vector<std::string> existingRows = AllRowsInBucket(category, tagColumn, tagValueSql);

    HsGenVerdict verdict = Hs_EvaluateCandidate(candidateText, existingRows, cardGated);
    if (!verdict.accepted)
        return verdict;

    std::string escapedText = candidateText;
    CharacterDatabase.EscapeString(escapedText);
    std::string escapedModel = model;
    CharacterDatabase.EscapeString(escapedModel);
    std::string escapedVersion = promptVersion;
    CharacterDatabase.EscapeString(escapedVersion);

    // NULL rather than an empty string for either -- hand-authored rows use
    // NULL, and an empty string would read as a set-but-blank value instead
    // of "not supplied" (e.g. a generator configured with no model name, or
    // a GM capture's prompt_version).
    std::string modelSql   = escapedModel.empty()   ? "NULL" : ("'" + escapedModel + "'");
    std::string versionSql = escapedVersion.empty() ? "NULL" : ("'" + escapedVersion + "'");

    if (tagColumn.empty())
    {
        CharacterDatabase.Execute(
            "INSERT INTO hside_corpus (name, text, generated_at, model, prompt_version) VALUES ('{}', '{}', NOW(), {}, {})",
            category, escapedText, modelSql, versionSql);
    }
    else
    {
        CharacterDatabase.Execute(
            "INSERT INTO hside_corpus (name, text, {}, generated_at, model, prompt_version) VALUES ('{}', '{}', {}, NOW(), {}, {})",
            tagColumn, category, escapedText, tagValueSql, modelSql, versionSql);
    }

    return { true, "" };
}
