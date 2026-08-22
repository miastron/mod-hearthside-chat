#include "hs_handler.h"
#include "hs_arbiter.h"
#include "hs_archetype.h"
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

#include "DBCStores.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h" // NewRpgStatus -- rpgInfo.GetStatus(), new 2026-08-21
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

    // §3 tier 0 / step 9: once a bot is selected, the reflex pattern table
    // is checked before anything else. A match is a complete answer, not a
    // fallback trigger — it never falls through to inference even when the
    // trigger also happens to be something an LLM could answer (§4.15:
    // "reflex is arbitrated like anything else; only the tier it resolves
    // to differs"). Gated by its own MaxTier.Reflex ceiling, independent of
    // MaxTier.DirectReply below — an operator can turn off canned replies
    // without touching the LLM ceiling, or vice versa.
    void TryReflex(Player* bot, Player* sender, const std::string& msg, bool isWhisper,
                    uint64_t botGuid, uint64_t senderGuid, bool inCombat, uint8_t botLevel, bool& handled)
    {
        handled = false;

        HsTier reflexCeiling = HsParseTier(g_HsMaxTierReflex);
        if (!HsTierAllows(reflexCeiling, HsTier::Reflex))
            return;

        HsReflexMatch match = Hs_MatchReflex(msg, botGuid, senderGuid, Hs_ParseBotQuestionMode(g_HsBotQuestionMode));
        if (match.kind == HsReflexKind::None)
            return;

        handled = true; // matched — handled whether or not a reply actually goes out
        if (match.text.empty())
            return; // Silent BotQuestion mode, or PersonalProbe's no-reply roll

        // Same style pass the LLM path applies (§4.18: "the style pass still
        // applies... same stock joke, different people telling it"), so a
        // reflex reply reads as this bot's voice rather than a flat string.
        // No history append and no cooldown/last-reply bump — tier 0 writes
        // no identity state at all (§4.12, §4.15).
        HsArchetype archetype = Hs_ArchetypeForBot(botGuid, botLevel);
        const HsArchetypeInfo& archetypeInfo = Hs_ArchetypeInfoFor(archetype);
        HsStyleContext styleCtx;
        styleCtx.baselineCare         = archetypeInfo.care;
        styleCtx.abbrevOverrideChance = archetypeInfo.hasAbbrevOverride ? archetypeInfo.abbrevOverrideChance : -1.0f;
        styleCtx.inCombat             = inCombat;
        styleCtx.verbalTic            = Hs_LookupCardSnapshot(botGuid).verbalTic;
        HsStyleResult style = Hs_ApplyStyle(botGuid, bot->GetName(), sender->GetName(), match.text, styleCtx);
        Hs_DeliverReflexReply(botGuid, senderGuid, isWhisper, style.text);
    }

    // §4.20 / step 10: the fourth answer source, sitting between the reflex
    // check and the tier ceiling -- the §3 decision flow's own note on where
    // this branch belongs ("immediately after R{Reflex pattern?} and before
    // CEIL{Tier ceiling}"). Six questions the realm can already answer
    // truthfully from live Player* state -- mount, level, zone, guild,
    // profession, gear -- each a direct lookup and a short template, no GPU
    // work and no chance of invention. Same "no identity state" shape as
    // TryReflex above: style pass applies, nothing is scored or written to
    // history, delivered through the same short-delay path.
    //
    // Returns false (falls through to the normal ceiling/LLM path) when the
    // trigger doesn't match any of the six, when it matches Mount but the
    // bot isn't currently mounted, or when g_HsGroundedAnswersEnabled is
    // off. A bot claiming a mount that isn't observably there is exactly
    // the kind of state-contradicting claim §4.13 exists to catch, so it's
    // simpler to let the normal path handle that case than to special-case
    // a "not mounted" reply here.
    bool TryGrounded(Player* bot, Player* sender, const std::string& msg, bool isWhisper,
                      uint64_t botGuid, uint64_t senderGuid, bool inCombat, uint8_t botLevel)
    {
        if (!g_HsGroundedAnswersEnabled)
            return false;

        HsGroundedKind kind = Hs_MatchGroundedQuestion(msg);
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
                // Primary professions only (§4.20's "profession skill") --
                // deliberately excludes secondary skills (fishing, cooking,
                // first aid), which is what a player asking "what
                // professions do you have" means in practice.
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
            // §4.12/§4.20 step 15's fifth fact class -- card_facts, only
            // resolvable for a carded (ring-3) bot. Hs_LookupCardFactField
            // returns "" for both "no active card" and "field absent",
            // which collapses to hasFact=false below and falls through
            // exactly like Mount's "not observably mounted" case -- no
            // invented lacks-line for a bot with no fact sheet at all.
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
                // Enum value -> natural phrasing, same "resolve before
                // templating" split hs_handler.cpp already owns for the six
                // realm-state kinds above (e.g. Mount's raw spell name).
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
            // §4.12/§4.20 step 16's fourth fact class -- hside_memory,
            // pair-scoped (bot, sender) rather than per-bot like the card
            // facts above. Never carries a timeline into the prompt; this
            // is the same zero-inference lookup-and-template path as
            // everything else in TryGrounded.
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

        // Same style pass and delivery path as TryReflex -- §4.20 frames
        // this as "the same *answer without the GPU* family as step 9".
        HsArchetype             archetype     = Hs_ArchetypeForBot(botGuid, botLevel);
        const HsArchetypeInfo&  archetypeInfo = Hs_ArchetypeInfoFor(archetype);
        HsStyleContext styleCtx;
        styleCtx.baselineCare         = archetypeInfo.care;
        styleCtx.abbrevOverrideChance = archetypeInfo.hasAbbrevOverride ? archetypeInfo.abbrevOverrideChance : -1.0f;
        styleCtx.inCombat             = inCombat;
        styleCtx.verbalTic            = Hs_LookupCardSnapshot(botGuid).verbalTic;
        HsStyleResult style = Hs_ApplyStyle(botGuid, bot->GetName(), sender->GetName(), reply, styleCtx);
        Hs_DeliverReflexReply(botGuid, senderGuid, isWhisper, style.text);
        return true;
    }

    // §3's CEIL->corpus branch: reached only when the surface's ceiling
    // permits corpus but not inference (an operator running the realm
    // cheaper). Same "answer without the GPU" shape as TryReflex/
    // TryGrounded above -- no bucket, no cooldown, no history/identity
    // write, since none of those exist to protect a zero-GPU-cost path.
    // Hs_SelectCorpusLine (hs_corpus.h) does the weighted anti-repeat pick
    // and its own exposure bookkeeping; this just applies the style pass
    // and delivers, identically to the two tiers above it.
    bool TryCorpusFallback(Player* bot, Player* sender, bool isWhisper,
                            uint64_t botGuid, uint64_t senderGuid, bool inCombat, uint8_t botLevel)
    {
        bool hasActiveCard = Hs_HasActiveCard(botGuid);
        std::string line = Hs_SelectCorpusLine(bot->getClass(), botLevel, hasActiveCard);
        if (line.empty())
            return false;

        // §4.7's card-only placeholders (%main_focus, %current_goal), first
        // reachable now that step 15 supplies real fact values. A no-op for
        // the overwhelming majority of lines, which never contain either
        // token -- Hs_ResolveCardPlaceholders is a plain substring replace.
        HsCardSnapshot snapshot = Hs_LookupCardSnapshot(botGuid);
        if (hasActiveCard)
        {
            std::string mainFocus   = Hs_LookupCardFactField(botGuid, "main_focus");
            std::string currentGoal = Hs_LookupCardFactField(botGuid, "current_goal");
            line = Hs_ResolveCardPlaceholders(line, mainFocus, currentGoal);
        }

        HsArchetype             archetype     = Hs_ArchetypeForBot(botGuid, botLevel);
        const HsArchetypeInfo&  archetypeInfo = Hs_ArchetypeInfoFor(archetype);
        HsStyleContext styleCtx;
        styleCtx.baselineCare         = archetypeInfo.care;
        styleCtx.abbrevOverrideChance = archetypeInfo.hasAbbrevOverride ? archetypeInfo.abbrevOverrideChance : -1.0f;
        styleCtx.inCombat             = inCombat;
        styleCtx.verbalTic            = snapshot.verbalTic;
        HsStyleResult style = Hs_ApplyStyle(botGuid, bot->GetName(), sender->GetName(), line, styleCtx);
        Hs_DeliverReflexReply(botGuid, senderGuid, isWhisper, style.text);
        return true;
    }

    // §4.15 step 5, and §4.14's "a ceiling is permission, not budget": once
    // reflex has passed on the trigger, check the surface's tier ceiling,
    // then the real admission gates (bucket/cooldown/breaker/queue depth)
    // inside Hs_TryEnqueue. Both /say and whisper are "direct address...
    // replies to player speech" (§3's tier table), so both read
    // MaxTier.DirectReply.
    //
    // A ceiling below inference but at or above corpus falls back to
    // TryCorpusFallback rather than silence -- corpus selection landed
    // ahead of step 12 specifically to close this gap (PROGRESS.md's
    // "Next" note after step 11). Below corpus, or if the corpus pick comes
    // back empty, the request is simply not admitted -- silence, §1's
    // retreat rule.
    void TryDispatch(Player* bot, Player* sender, const std::string& msg, bool isWhisper)
    {
        uint64_t botGuid    = bot->GetGUID().GetRawValue();
        uint64_t senderGuid = sender->GetGUID().GetRawValue();
        bool     inCombat   = bot->IsInCombat(); // §4.11: context modulates care downward in combat
        uint8_t  botLevel   = bot->GetLevel();   // §4.13: archetype eligibility filter

        // New 2026-08-21: the bot's live mod-playerbots activity, read here
        // (only the world thread may touch PlayerbotAI*) and carried into
        // the queued request as a plain enum value so the worker thread can
        // fold it into the prompt without touching a game object off-thread
        // -- same pattern inCombat/botLevel already use.
        NewRpgStatus rpgStatus = RPG_IDLE;
        if (PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
            rpgStatus = botAI->rpgInfo.GetStatus();

        bool reflexHandled = false;
        TryReflex(bot, sender, msg, isWhisper, botGuid, senderGuid, inCombat, botLevel, reflexHandled);
        if (reflexHandled)
            return;

        if (TryGrounded(bot, sender, msg, isWhisper, botGuid, senderGuid, inCombat, botLevel))
            return;

        HsTier ceiling = HsParseTier(g_HsMaxTierDirectReply);
        if (!HsTierAllows(ceiling, HsTier::Inference))
        {
            if (HsTierAllows(ceiling, HsTier::Corpus))
                TryCorpusFallback(bot, sender, isWhisper, botGuid, senderGuid, inCombat, botLevel);
            return;
        }

        if (!Hs_TryEnqueue(botGuid, bot->GetName(), senderGuid, sender->GetName(), isWhisper, msg, inCombat, botLevel, rpgStatus, /*isFollowUp=*/false) && g_HsDebugEnabled)
            LOG_INFO("server.loading", "[HearthsideChat] Enqueue rejected for bot {}.", bot->GetName());
    }
}

bool HsChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg)
{
    if (!g_HsEnable || type != CHAT_MSG_SAY || msg.empty())
        return true;

    if (!player || IsBot(player))
        return true; // sender-aware (bot-initiated) chatter is not wired up yet — see hs_config.h

    // §4.16 step 14: a real player speaking aborts any scripted bot-to-bot
    // conversation they're witnessing -- "finish the line in flight, then
    // stop." Unconditional on the real-player branch, independent of
    // whether any bot ends up eligible to reply below.
    Hs_AbortScriptsWitnessedBy(player->GetGUID().GetRawValue());

    // New 2026-08-21 (Claude/PLAN-engagement.md): same reasoning, for a
    // scheduled engagement follow-up -- delivering one after the player has
    // already said something else reads as the bot not listening.
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
        if (candidate->GetDistance(player) > g_HsSayDistance)
            continue;
        eligible.push_back(candidate);
    }
    if (eligible.empty())
        return true;

    std::vector<Player*> selected = Hs_ArbitrateReplies(player, msg, eligible);
    for (Player* bot : selected)
        TryDispatch(bot, player, msg, /*isWhisper=*/false);

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

    // New 2026-08-21: whisper is now a full engagement-follow-up surface
    // (Claude/PLAN-engagement.md), unlike scripted bot-to-bot which is only
    // ever witnessed via /say -- so this handler needs the same abort-on-
    // interrupt call the /say path already has, which this one never did.
    Hs_AbortEngagementFollowUpsFor(player->GetGUID().GetRawValue());
    if (!IsBot(receiver))
        return true;
    if (Hs_IsExcludedBotName(receiver->GetName()))
        return true; // HearthsideChat.ExcludeNames -- never spoken through, no tier at all
    if (g_HsDisableRepliesInCombat && receiver->IsInCombat())
        return true;

    if (urand(0, 99) >= g_HsReplyChanceWhisper)
        return true;

    TryDispatch(receiver, player, msg, /*isWhisper=*/true);
    return true;
}

void HsDeliveryWorldScript::OnUpdate(uint32_t /*diff*/)
{
    Hs_DeliverPending();
}
