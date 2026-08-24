#include "hs_handler.h"
#include "hs_arbiter.h"
#include "hs_archetype.h"
#include "hs_channel.h"
#include "hs_config.h"
#include "hs_corpus.h"
#include "hs_engagement.h"
#include "hs_grounded.h"
#include "hs_identity_store.h"
#include "hs_memory_store.h"
#include "hs_queue.h"
#include "hs_reflex.h"
#include "hs_script.h"
#include "hs_style.h"
#include "hs_tier.h"
#include "hs_topic_gate.h"

#include "Channel.h"    // §4.17 channel hook: Channel::GetChannelId()/GetName()
#include "DBCStores.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h" // NewRpgStatus -- rpgInfo.GetStatus()
#include "PlayerbotMgr.h"
#include "ObjectAccessor.h"
#include "Random.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"

#include <string>
#include <utility>
#include <vector>

namespace
{
    bool IsBot(Player* p)
    {
        if (!p)
            return false;
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(p);
        return ai && ai->IsBotAI();
    }

    // §4.13's remaining topic-gate facts, read fresh per request like
    // inCombat/botLevel -- gear, group, and instance are all as volatile as
    // combat. hs_topic_gate.h stays pure/no-AC-dependency for standalone
    // testing, so this Player*-reading half lives here (and is duplicated
    // in hs_engagement.cpp's TryFireFollowUp) rather than there.
    HsTopicGateContext BuildTopicGateContext(Player* bot)
    {
        HsTopicGateContext ctx;
        ctx.avgItemLevel = static_cast<uint32_t>(bot->GetAverageItemLevel());

        if (Group* group = bot->GetGroup())
        {
            ctx.inGroup       = true;
            ctx.isGroupLeader = group->IsLeader(bot->GetGUID());
        }

        if (Map* map = bot->GetMap())
        {
            ctx.inInstance = map->IsDungeon() || map->IsRaid();
            if (ctx.inInstance)
                ctx.instanceName = map->GetMapName();
        }

        ctx.goldCopper = bot->GetMoney();

        if (AreaTableEntry const* entry = sAreaTableStore.LookupEntry(bot->GetZoneId()))
        {
            const char* name = entry->area_name[0];
            if (name && *name)
                ctx.zoneName = name;
        }

        return ctx;
    }

    // Once a bot is selected, the reflex pattern table is checked before
    // anything else. A match is a complete answer, not a fallback trigger --
    // it never falls through to inference even when the trigger also
    // happens to be something an LLM could answer. Gated by its own
    // MaxTier.Reflex ceiling, independent of MaxTier.DirectReply below --
    // an operator can turn off canned replies without touching the LLM
    // ceiling, or vice versa.
    void TryReflex(Player* bot, Player* sender, const std::string& msg, HsReplyChannel channel,
                    uint64_t botGuid, uint64_t senderGuid, bool inCombat, uint8_t botLevel, bool& handled)
    {
        handled = false;

        HsTier reflexCeiling = HsParseTier(g_HsMaxTierReflex);
        if (!HsTierAllows(reflexCeiling, HsTier::Reflex))
            return;

        HsReflexMatch match = Hs_MatchReflex(msg, botGuid, senderGuid, Hs_ParseBotQuestionMode(g_HsBotQuestionMode));
        if (match.kind == HsReflexKind::None)
            return;

        handled = true; // matched -- handled whether or not a reply actually goes out
        if (match.text.empty())
            return; // Silent BotQuestion mode, or PersonalProbe's no-reply roll

        // Same style pass the LLM path applies, so a reflex reply reads as
        // this bot's voice rather than a flat string. No history append and
        // no cooldown/last-reply bump -- tier 0 writes no identity state at
        // all.
        HsArchetype archetype = Hs_ArchetypeForBot(botGuid, botLevel);
        HsArchetypeInfo const archetypeInfo = Hs_ArchetypeInfoFor(archetype);
        HsStyleContext styleCtx;
        styleCtx.baselineCare         = archetypeInfo.care;
        styleCtx.abbrevOverrideChance = archetypeInfo.hasAbbrevOverride ? archetypeInfo.abbrevOverrideChance : -1.0f;
        styleCtx.inCombat             = inCombat;
        styleCtx.verbalTic            = Hs_LookupCardSnapshot(botGuid).verbalTic;
        styleCtx.tradeCareOffset      = Hs_TradeCareOffsetFor(botGuid);
        HsStyleResult style = Hs_ApplyStyle(botGuid, bot->GetName(), sender->GetName(), match.text, styleCtx);
        Hs_DeliverReflexReply(botGuid, senderGuid, channel, style.text);
    }

    // A fourth answer source, sitting between the reflex check and the tier
    // ceiling. Questions the realm can already answer truthfully from live
    // Player*/DB state -- mount, level, gold, zone, guild, profession, gear,
    // card facts, and shared-history recall (hs_grounded.h's HsGroundedKind)
    // -- each a direct lookup and a short template, no GPU work and no
    // chance of invention. Same "no identity state" shape as TryReflex
    // above: style pass applies, nothing is scored or written to history,
    // delivered through the same short-delay path.
    //
    // Returns false (falls through to the normal ceiling/LLM path) when the
    // trigger doesn't match any loaded question, when it matches Mount but
    // the bot isn't currently mounted, or when g_HsGroundedAnswersEnabled is
    // off. A bot claiming a mount that isn't observably there would be a
    // state-contradicting claim, so it's simpler to let the normal path
    // handle that case than to special-case a "not mounted" reply here.
    bool TryGrounded(Player* bot, Player* sender, const std::string& msg, HsReplyChannel channel,
                      uint64_t botGuid, uint64_t senderGuid, bool inCombat, uint8_t botLevel)
    {
        if (!g_HsGroundedAnswersEnabled)
            return false;

        HsGroundedKind kind = Hs_MatchGroundedQuestion(msg, g_HsGroundedFuzzyMaxDistance);
        if (kind == HsGroundedKind::None)
            return false;

        bool        hasFact = true;
        std::string fact;

        switch (kind)
        {
            case HsGroundedKind::Mount:
            {
                if (!bot->IsMounted())
                    return false; // not observably mounted -- fall through rather than claim one
                auto const& mountAuras = bot->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
                if (mountAuras.empty())
                    return false; // defensive: IsMounted() true but no aura found
                const char* name = mountAuras.front()->GetSpellInfo()->SpellName[0];
                fact = (name && *name) ? name : "a mount";
                break;
            }
            case HsGroundedKind::Level:
            {
                fact = std::to_string(bot->GetLevel());
                break;
            }
            case HsGroundedKind::Gold:
            {
                // Same gold/silver split hs_topic_gate.cpp uses for the
                // persona-line fact. Rounds to whatever denomination a
                // player would actually say ("47g", "12s"), not an exact
                // copper count; always resolvable, even at 0 gold.
                uint32_t copper = bot->GetMoney();
                uint32_t gold   = copper / 10000;
                uint32_t silver = (copper / 100) % 100;
                fact = gold > 0 ? (std::to_string(gold) + "g") : (std::to_string(silver) + "s");
                break;
            }
            case HsGroundedKind::Zone:
            {
                AreaTableEntry const* entry = sAreaTableStore.LookupEntry(bot->GetZoneId());
                const char* name = entry ? entry->area_name[0] : nullptr;
                fact = (name && *name) ? name : "around here";
                break;
            }
            case HsGroundedKind::Guild:
            {
                uint32_t guildId = bot->GetGuildId();
                Guild*   guild   = guildId ? sGuildMgr->GetGuildById(guildId) : nullptr;
                hasFact = (guild != nullptr);
                if (hasFact)
                    fact = guild->GetName();
                break;
            }
            case HsGroundedKind::Profession:
            {
                // Primary professions only -- deliberately excludes
                // secondary skills (fishing, cooking, first aid), which is
                // what a player asking "what professions do you have"
                // means in practice.
                static const std::pair<uint32_t, const char*> kProfessions[] = {
                    { SKILL_ALCHEMY, "alchemy" },       { SKILL_BLACKSMITHING, "blacksmithing" },
                    { SKILL_ENCHANTING, "enchanting" }, { SKILL_ENGINEERING, "engineering" },
                    { SKILL_HERBALISM, "herbalism" },   { SKILL_INSCRIPTION, "inscription" },
                    { SKILL_JEWELCRAFTING, "jewelcrafting" }, { SKILL_LEATHERWORKING, "leatherworking" },
                    { SKILL_MINING, "mining" },         { SKILL_SKINNING, "skinning" },
                    { SKILL_TAILORING, "tailoring" },
                };
                std::vector<std::string> known;
                for (auto const& prof : kProfessions)
                {
                    if (bot->HasSkill(prof.first))
                        known.push_back(std::string(prof.second) + " (" +
                                         std::to_string(bot->GetSkillValue(prof.first)) + ")");
                }
                hasFact = !known.empty();
                if (hasFact)
                {
                    fact = known[0];
                    if (known.size() > 1)
                        fact += " and " + known[1]; // WotLK caps a character at two primary professions
                }
                break;
            }
            case HsGroundedKind::Gear:
            {
                // Priority order, not every slot -- the visually notable
                // pieces a player would actually be commenting on.
                static const uint8_t kGearSlots[] = {
                    EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_CHEST, EQUIPMENT_SLOT_LEGS, EQUIPMENT_SLOT_HEAD,
                };
                hasFact = false;
                for (uint8_t slot : kGearSlots)
                {
                    if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    {
                        if (ItemTemplate const* tmpl = item->GetTemplate())
                        {
                            fact    = tmpl->Name1;
                            hasFact = true;
                            break;
                        }
                    }
                }
                break;
            }
            // Card facts, only resolvable for a carded (ring-3) bot.
            // Hs_LookupCardFactField returns "" for both "no active card"
            // and "field absent", which collapses to hasFact=false below
            // and falls through exactly like Mount's "not observably
            // mounted" case -- no invented lacks-line for a bot with no
            // fact sheet at all.
            case HsGroundedKind::CurrentGoal:
            {
                fact    = Hs_LookupCardFactField(botGuid, "current_goal");
                hasFact = !fact.empty();
                break;
            }
            case HsGroundedKind::PlayedSince:
            {
                std::string raw = Hs_LookupCardFactField(botGuid, "played_since");
                hasFact = !raw.empty();
                // Enum value -> natural phrasing, same resolve-before-
                // templating split used for the six realm-state kinds
                // above (e.g. Mount's raw spell name).
                if (raw == "vanilla")      fact = "vanilla";
                else if (raw == "bc")      fact = "BC";
                else if (raw == "wrath")   fact = "Wrath";
                else                       hasFact = false; // unrecognized value -- fall through rather than echo it raw
                break;
            }
            case HsGroundedKind::Alt:
            {
                fact    = Hs_LookupCardFactField(botGuid, "alt");
                hasFact = !fact.empty();
                break;
            }
            // hside_memory-backed facts, pair-scoped (bot, sender) rather
            // than per-bot like the card facts above. Never carries a
            // timeline into the prompt -- same zero-inference
            // lookup-and-template path as everything else in TryGrounded.
            case HsGroundedKind::RecallMet:
            {
                hasFact = Hs_HasMetBefore(botGuid, senderGuid);
                break;
            }
            case HsGroundedKind::RecallDungeon:
            {
                HsMemoryFact memFact = Hs_LookupLastDungeonRun(botGuid, senderGuid);
                hasFact = memFact.hasFact;
                fact    = memFact.text;
                break;
            }
            case HsGroundedKind::RecallGrouped:
            {
                hasFact = Hs_HasGroupedBefore(botGuid, senderGuid);
                break;
            }
            case HsGroundedKind::None:
                return false; // unreachable -- guarded above
        }

        std::string reply = Hs_BuildGroundedReply(kind, hasFact, fact, botGuid, msg);
        if (reply.empty())
            return false;

        // Same style pass and delivery path as TryReflex -- both answer
        // without touching the GPU.
        HsArchetype             archetype     = Hs_ArchetypeForBot(botGuid, botLevel);
        HsArchetypeInfo const   archetypeInfo = Hs_ArchetypeInfoFor(archetype);
        HsStyleContext styleCtx;
        styleCtx.baselineCare         = archetypeInfo.care;
        styleCtx.abbrevOverrideChance = archetypeInfo.hasAbbrevOverride ? archetypeInfo.abbrevOverrideChance : -1.0f;
        styleCtx.inCombat             = inCombat;
        styleCtx.verbalTic            = Hs_LookupCardSnapshot(botGuid).verbalTic;
        styleCtx.tradeCareOffset      = Hs_TradeCareOffsetFor(botGuid);
        HsStyleResult style = Hs_ApplyStyle(botGuid, bot->GetName(), sender->GetName(), reply, styleCtx);
        Hs_DeliverReflexReply(botGuid, senderGuid, channel, style.text);
        return true;
    }

    // Reached only when the surface's ceiling permits corpus but not
    // inference (an operator running the realm cheaper). Same "answer
    // without the GPU" shape as TryReflex/TryGrounded above -- no bucket,
    // no cooldown, no history/identity write, since none of those exist
    // to protect a zero-GPU-cost path. Hs_SelectCorpusLine (hs_corpus.h)
    // does the weighted anti-repeat pick and its own exposure bookkeeping;
    // this just applies the style pass and delivers, identically to the
    // two tiers above it.
    bool TryCorpusFallback(Player* bot, Player* sender, HsReplyChannel channel,
                            uint64_t botGuid, uint64_t senderGuid, bool inCombat, uint8_t botLevel)
    {
        bool hasActiveCard = Hs_HasActiveCard(botGuid);
        std::string line = Hs_SelectCorpusLine(bot->getClass(), botLevel, static_cast<uint8_t>(bot->GetTeamId()),
                                                bot->GetZoneId(), hasActiveCard);
        if (line.empty())
            return false;

        // Card-only placeholders (%main_focus, %current_goal). A no-op for
        // the overwhelming majority of lines, which never contain either
        // token -- Hs_ResolveCardPlaceholders is a plain substring replace.
        HsCardSnapshot snapshot = Hs_LookupCardSnapshot(botGuid);
        if (hasActiveCard)
        {
            std::string mainFocus   = Hs_LookupCardFactField(botGuid, "main_focus");
            std::string currentGoal = Hs_LookupCardFactField(botGuid, "current_goal");
            line = Hs_ResolveCardPlaceholders(line, mainFocus, currentGoal);
        }

        // Universal placeholders (%item_link, %class, %level, %zone,
        // %guild, %quest_link), after the card pass so the leftover check
        // sees a fully-substituted line. Guarded on a bare '%' so the
        // common no-placeholder case skips the bag/quest-log scans
        // Hs_BuildPlaceholderContext does. An unresolvable placeholder
        // drops the line into silence rather than an untrue claim (§4.13).
        if (line.find('%') != std::string::npos)
        {
            if (!Hs_ResolveUniversalPlaceholders(line, Hs_BuildPlaceholderContext(bot)))
                return false;
        }

        HsArchetype             archetype     = Hs_ArchetypeForBot(botGuid, botLevel);
        HsArchetypeInfo const   archetypeInfo = Hs_ArchetypeInfoFor(archetype);
        HsStyleContext styleCtx;
        styleCtx.baselineCare         = archetypeInfo.care;
        styleCtx.abbrevOverrideChance = archetypeInfo.hasAbbrevOverride ? archetypeInfo.abbrevOverrideChance : -1.0f;
        styleCtx.inCombat             = inCombat;
        styleCtx.verbalTic            = snapshot.verbalTic;
        styleCtx.tradeCareOffset      = Hs_TradeCareOffsetFor(botGuid);
        HsStyleResult style = Hs_ApplyStyle(botGuid, bot->GetName(), sender->GetName(), line, styleCtx);
        Hs_DeliverReflexReply(botGuid, senderGuid, channel, style.text);
        return true;
    }

    // §4.17: maps a live Channel* to one of the seven kinds this module
    // knows about, via mod-playerbots' own ChatChannelId enum (PlayerbotAI.h
    // -- the same ids mod-playerbots itself joins bots to, PlayerbotMgr.cpp)
    // rather than parsing Channel::GetName()'s zone-suffixed display string.
    // World has no ChatChannels.dbc entry in this build (mod-playerbots
    // joins it via a raw CMSG_JOIN_CHANNEL with id 0), so it's matched by
    // name instead. Returns false for any other channel (a GM/custom
    // channel, or a DBC id this module doesn't have a policy for) -- the
    // hook passes those through untouched.
    bool ChannelKindFor(Channel* channel, HsChannelKind& out)
    {
        if (channel->GetName() == "World")
        {
            out = HsChannelKind::World;
            return true;
        }

        switch (channel->GetChannelId())
        {
            case ChatChannelId::TRADE:              out = HsChannelKind::Trade;            return true;
            case ChatChannelId::GENERAL:             out = HsChannelKind::General;          return true;
            case ChatChannelId::LOOKING_FOR_GROUP:   out = HsChannelKind::LookingForGroup;  return true;
            case ChatChannelId::GUILD_RECRUITMENT:   out = HsChannelKind::GuildRecruitment; return true;
            case ChatChannelId::LOCAL_DEFENSE:       out = HsChannelKind::LocalDefense;     return true;
            case ChatChannelId::WORLD_DEFENSE:       out = HsChannelKind::WorldDefense;     return true;
            default: return false;
        }
    }

    // §4.17's corpus-only channel reply -- deliberately not a call into
    // TryDispatch: channel content is corpus/script only by design (never
    // tier-2 inference, never reflex/grounded, which are direct-address
    // concepts that don't fit ambient channel chatter), so this mirrors only
    // TryCorpusFallback's shape, scoped to Hs_SelectChannelLine instead of
    // Hs_SelectCorpusLine and HsReplyChannel::Channel instead of `channel`.
    void TryChannelCorpusReply(Player* bot, Player* sender, HsChannelKind kind,
                                uint64_t botGuid, uint64_t senderGuid, uint8_t botLevel)
    {
        std::string line = Hs_SelectChannelLine(kind, bot->getClass(), botLevel,
                                                 static_cast<uint8_t>(bot->GetTeamId()), bot->GetZoneId());
        if (line.empty())
            return;

        // Universal placeholders only -- channel_* categories are never
        // card_gated (hs_corpus.cpp's ChannelColumnFor query), so no card
        // placeholder pass is needed here, unlike TryCorpusFallback.
        if (line.find('%') != std::string::npos)
        {
            if (!Hs_ResolveUniversalPlaceholders(line, Hs_BuildPlaceholderContext(bot)))
                return;
        }

        HsArchetype             archetype     = Hs_ArchetypeForBot(botGuid, botLevel);
        HsArchetypeInfo const   archetypeInfo = Hs_ArchetypeInfoFor(archetype);
        HsStyleContext styleCtx;
        styleCtx.baselineCare         = archetypeInfo.care;
        styleCtx.abbrevOverrideChance = archetypeInfo.hasAbbrevOverride ? archetypeInfo.abbrevOverrideChance : -1.0f;
        styleCtx.inCombat             = bot->IsInCombat();
        styleCtx.verbalTic            = Hs_LookupCardSnapshot(botGuid).verbalTic;
        styleCtx.tradeCareOffset      = Hs_TradeCareOffsetFor(botGuid);
        HsStyleResult style = Hs_ApplyStyle(botGuid, bot->GetName(), sender->GetName(), line, styleCtx);
        Hs_DeliverReflexReply(botGuid, senderGuid, HsReplyChannel::Channel, style.text, kind);
    }

    // Once reflex/grounded have passed on the trigger, check the surface's
    // tier ceiling, then the real admission gates (bucket/cooldown/
    // breaker/queue depth) inside Hs_TryEnqueue. A ceiling is permission,
    // not budget. Say, whisper, party/raid, and guild all read
    // MaxTier.DirectReply -- the ceiling is one surface-shaped concept ("a
    // player addressed a bot"), not per-channel; only delivery and the
    // interaction_score weight (hs_identity.h) vary by channel.
    //
    // A ceiling below inference but at or above corpus falls back to
    // TryCorpusFallback rather than silence. Below corpus, or if the
    // corpus pick comes back empty, the request is simply not admitted --
    // silence.
    void TryDispatch(Player* bot, Player* sender, const std::string& msg, HsReplyChannel channel)
    {
        uint64_t botGuid    = bot->GetGUID().GetRawValue();
        uint64_t senderGuid = sender->GetGUID().GetRawValue();
        bool     inCombat   = bot->IsInCombat(); // context modulates care downward in combat
        uint8_t  botLevel   = bot->GetLevel();   // archetype eligibility filter

        // The bot's live mod-playerbots activity, read here (only the
        // world thread may touch PlayerbotAI*) and carried into the queued
        // request as a plain enum value so the worker thread can fold it
        // into the prompt without touching a game object off-thread --
        // same pattern inCombat/botLevel already use.
        NewRpgStatus rpgStatus = RPG_IDLE;
        if (PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
            rpgStatus = botAI->rpgInfo.GetStatus();

        bool reflexHandled = false;
        TryReflex(bot, sender, msg, channel, botGuid, senderGuid, inCombat, botLevel, reflexHandled);
        if (reflexHandled)
            return;

        if (TryGrounded(bot, sender, msg, channel, botGuid, senderGuid, inCombat, botLevel))
            return;

        HsTier ceiling = HsParseTier(g_HsMaxTierDirectReply);
        if (!HsTierAllows(ceiling, HsTier::Inference))
        {
            if (HsTierAllows(ceiling, HsTier::Corpus))
                TryCorpusFallback(bot, sender, channel, botGuid, senderGuid, inCombat, botLevel);
            return;
        }

        // §4.13's remaining topic-gate facts. Only read here, not for the
        // reflex/grounded/corpus tiers above -- those never reach the LLM
        // prompt this feeds, so the read would be wasted work.
        HsTopicGateContext topicGate = BuildTopicGateContext(bot);

        if (!Hs_TryEnqueue(botGuid, bot->GetName(), senderGuid, sender->GetName(), channel, msg, inCombat, botLevel, rpgStatus, topicGate, /*isFollowUp=*/false) && g_HsDebugEnabled)
            LOG_INFO("server.loading", "[HearthsideChat] Enqueue rejected for bot {}.", bot->GetName());
    }
}

bool HsChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg)
{
    if (!g_HsEnable || type != CHAT_MSG_SAY || msg.empty())
        return true;

    if (!player || IsBot(player))
        return true; // sender-aware (bot-initiated) chatter is not wired up yet -- see hs_config.h

    // A real player speaking aborts any scripted bot-to-bot conversation
    // they're witnessing -- finish the line in flight, then stop.
    // Unconditional on the real-player branch, independent of whether any
    // bot ends up eligible to reply below.
    Hs_AbortScriptsWitnessedBy(player->GetGUID().GetRawValue());

    // Same reasoning, for a scheduled engagement follow-up -- delivering
    // one after the player has already said something else reads as the
    // bot not listening.
    Hs_AbortEngagementFollowUpsFor(player->GetGUID().GetRawValue());

    std::vector<Player*> eligible;
    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        Player* candidate = itr.second;
        if (!candidate || candidate == player || !candidate->IsInWorld())
            continue;
        if (!IsBot(candidate))
            continue;
        if (Hs_IsExcludedBotName(candidate->GetName()))
            continue; // HearthsideChat.ExcludeNames -- never spoken through, no tier at all
        if (candidate->GetTeamId() != player->GetTeamId())
            continue; // opposing faction can't read /say
        if (g_HsDisableRepliesInCombat && candidate->IsInCombat())
            continue;
        // IsWithinDistInMap, not GetDistance: the candidate loop walks
        // ObjectAccessor::GetPlayers(), which is realm-wide, and GetDistance
        // is purely positional -- no map, no phase. Two instances of the same
        // dungeon share one coordinate space, so the bare distance would make
        // a bot in another instance "nearby".
        if (!candidate->IsWithinDistInMap(player, g_HsSayDistance))
            continue;
        eligible.push_back(candidate);
    }
    if (eligible.empty())
        return true;

    std::vector<Player*> selected = Hs_ArbitrateReplies(player, msg, eligible);
    for (Player* bot : selected)
        TryDispatch(bot, player, msg, HsReplyChannel::Say);

    return true;
}

bool HsChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Player* receiver)
{
    if (!g_HsEnable || type != CHAT_MSG_WHISPER || msg.empty())
        return true;

    if (!player || !receiver || player == receiver)
        return true;
    if (IsBot(player))
        return true; // sender-aware (bot-initiated) chatter is not wired up yet

    // Whisper is a full engagement-follow-up surface, unlike scripted
    // bot-to-bot which is only ever witnessed via /say -- so this handler
    // needs the same abort-on-interrupt call the /say path already has.
    Hs_AbortEngagementFollowUpsFor(player->GetGUID().GetRawValue());
    if (!IsBot(receiver))
        return true;
    if (Hs_IsExcludedBotName(receiver->GetName()))
        return true; // HearthsideChat.ExcludeNames -- never spoken through, no tier at all
    if (g_HsDisableRepliesInCombat && receiver->IsInCombat())
        return true;

    if (urand(0, 99) >= g_HsReplyChanceWhisper)
        return true;

    TryDispatch(receiver, player, msg, HsReplyChannel::Whisper);
    return true;
}

// CHAT_MSG_PARTY/PARTY_LEADER stay within the sender's own subgroup even in
// a raid (ChatHandler.cpp's HandleMessagechatOpcode broadcasts by subgroup,
// not the whole group) -- a candidate outside it never heard the line, so
// it can't "reply." CHAT_MSG_RAID/RAID_LEADER reach the whole raid, no
// subgroup filter. RAID_WARNING and the battleground variants are
// deliberately excluded: a leader broadcast or BG channel isn't something a
// bot chimes into.
bool HsChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Group* group)
{
    if (!g_HsEnable || msg.empty() || !player || !group)
        return true;
    if (type != CHAT_MSG_PARTY && type != CHAT_MSG_PARTY_LEADER &&
        type != CHAT_MSG_RAID && type != CHAT_MSG_RAID_LEADER)
        return true;
    if (IsBot(player))
        return true; // sender-aware (bot-initiated) chatter is not wired up yet

    Hs_AbortEngagementFollowUpsFor(player->GetGUID().GetRawValue());

    bool subgroupScoped = (type == CHAT_MSG_PARTY || type == CHAT_MSG_PARTY_LEADER);

    std::vector<Player*> eligible;
    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        Player* candidate = itr.second;
        if (!candidate || candidate == player || !candidate->IsInWorld())
            continue;
        if (!IsBot(candidate))
            continue;
        if (candidate->GetGroup() != group)
            continue;
        if (subgroupScoped && !group->SameSubGroup(player, candidate))
            continue;
        if (Hs_IsExcludedBotName(candidate->GetName()))
            continue; // HearthsideChat.ExcludeNames -- never spoken through, no tier at all
        if (g_HsDisableRepliesInCombat && candidate->IsInCombat())
            continue;
        eligible.push_back(candidate);
    }
    if (eligible.empty())
        return true;

    HsReplyChannel channel = (type == CHAT_MSG_RAID || type == CHAT_MSG_RAID_LEADER)
        ? HsReplyChannel::Raid : HsReplyChannel::Party;

    std::vector<Player*> selected = Hs_ArbitrateReplies(player, msg, eligible);
    for (Player* bot : selected)
        TryDispatch(bot, player, msg, channel);

    return true;
}

bool HsChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Guild* guild)
{
    if (!g_HsEnable || type != CHAT_MSG_GUILD || msg.empty() || !player || !guild)
        return true;
    if (IsBot(player))
        return true; // sender-aware (bot-initiated) chatter is not wired up yet

    Hs_AbortEngagementFollowUpsFor(player->GetGUID().GetRawValue());

    uint32_t guildId = guild->GetId();
    std::vector<Player*> eligible;
    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        Player* candidate = itr.second;
        if (!candidate || candidate == player || !candidate->IsInWorld())
            continue;
        if (!IsBot(candidate))
            continue;
        if (candidate->GetGuildId() != guildId)
            continue;
        if (Hs_IsExcludedBotName(candidate->GetName()))
            continue; // HearthsideChat.ExcludeNames -- never spoken through, no tier at all
        if (g_HsDisableRepliesInCombat && candidate->IsInCombat())
            continue;
        eligible.push_back(candidate);
    }
    if (eligible.empty())
        return true;

    std::vector<Player*> selected = Hs_ArbitrateReplies(player, msg, eligible);
    for (Player* bot : selected)
        TryDispatch(bot, player, msg, HsReplyChannel::Guild);

    return true;
}

// §4.17: mod-playerbots joins every bot to every one of these channels
// unconditionally (trap 20) -- this is the only gate deciding whether any of
// them ever speaks. Corpus-only by design (TryChannelCorpusReply above,
// never TryDispatch), rate-limited per channel (Hs_ChannelBucketTake, checked
// before any candidate scan so a throttled channel costs nothing), and
// sampled rather than enumerated (Hs_OrderChannelCandidates, trap 21) -- a
// channel with most of the realm in it has no proximity bound the way /say
// does, so nothing else here caps how many bots could otherwise be rolled
// against reply chance on one message.
bool HsChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel)
{
    if (!g_HsEnable || type != CHAT_MSG_CHANNEL || msg.empty() || !player || !channel)
        return true;
    if (IsBot(player))
        return true; // sender-aware (bot-initiated) chatter is not wired up yet

    HsChannelKind kind;
    if (!ChannelKindFor(channel, kind))
        return true; // a GM/custom channel, or a DBC id this module has no policy for

    HsChannelPolicy policy = Hs_ChannelPolicyFor(kind);
    if (!HsTierAllows(policy.maxTier, HsTier::Corpus))
        return true;

    // §4.17's Trade `care` offset trigger: stamps every bot currently a
    // member of this channel instance, independent of whether any of them
    // go on to reply below -- "recently witnessed WTS/WTB activity" is true
    // for the whole channel, not just whichever bot happens to answer.
    if (kind == HsChannelKind::Trade && Hs_IsWtsWtb(msg))
    {
        for (auto const& itr : ObjectAccessor::GetPlayers())
        {
            Player* candidate = itr.second;
            if (candidate && candidate->IsInWorld() && IsBot(candidate) && candidate->IsInChannel(channel))
                Hs_NoteTradeSighting(candidate->GetGUID().GetRawValue());
        }
    }

    Hs_AbortScriptsWitnessedBy(player->GetGUID().GetRawValue());
    Hs_AbortEngagementFollowUpsFor(player->GetGUID().GetRawValue());

    if (!Hs_ChannelBucketTake(kind))
        return true; // throttled -- no candidate scan at all on a busy channel

    std::vector<Player*> eligible;
    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        Player* candidate = itr.second;
        if (!candidate || candidate == player || !candidate->IsInWorld())
            continue;
        if (!IsBot(candidate))
            continue;
        if (Hs_IsExcludedBotName(candidate->GetName()))
            continue; // HearthsideChat.ExcludeNames -- never spoken through, no tier at all
        if (candidate->GetTeamId() != player->GetTeamId())
            continue; // opposing faction can't read this channel
        if (g_HsDisableRepliesInCombat && candidate->IsInCombat())
            continue;
        if (!candidate->IsInChannel(channel))
            continue; // Channel's own member list is private; this is the public membership check
        eligible.push_back(candidate);
    }
    if (eligible.empty())
        return true;

    std::vector<HsChannelCandidate> candidates;
    candidates.reserve(eligible.size());
    for (Player* candidate : eligible)
        candidates.push_back({ candidate->GetGUID().GetRawValue(), candidate->GetZoneId() });

    uint64_t seed = (static_cast<uint64_t>(urand(0, 0xFFFFFFFFu)) << 32) | urand(0, 0xFFFFFFFFu);
    candidates = Hs_OrderChannelCandidates(candidates, player->GetZoneId(), policy.maxCandidates, seed);

    std::vector<Player*> capped;
    capped.reserve(candidates.size());
    for (HsChannelCandidate const& c : candidates)
        for (Player* candidate : eligible)
            if (candidate->GetGUID().GetRawValue() == c.guid)
            {
                capped.push_back(candidate);
                break;
            }
    if (capped.empty())
        return true;

    std::vector<Player*> selected = Hs_ArbitrateReplies(player, msg, capped);
    for (Player* bot : selected)
        TryChannelCorpusReply(bot, player, kind, bot->GetGUID().GetRawValue(), player->GetGUID().GetRawValue(), bot->GetLevel());

    return true;
}

void HsDeliveryWorldScript::OnUpdate(uint32_t /*diff*/)
{
    Hs_DeliverPending();
}
