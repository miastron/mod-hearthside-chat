#include "hs_command.h"
#include "hs_config.h"
#include "hs_corpus.h"
#include "hs_engagement.h"
#include "hs_gen_validate.h"
#include "hs_generator.h"
#include "hs_identity_store.h"
#include "hs_memory_store.h"
#include "hs_opener.h"
#include "hs_queue.h"
#include "hs_script.h"

#include "CharacterCache.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "DatabaseEnv.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "QueryResult.h"

#include <string>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    // Resolves a bot by name regardless of online status (§4.19's control
    // commands act on a named bot whether or not it's currently loaded,
    // unlike `.hearthside capture` above which needs a live Player* for the
    // bot's current class/level). Returns 0 if the name doesn't exist.
    uint64_t ResolveBotGuidByName(const std::string& name)
    {
        ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(name);
        return guid.IsEmpty() ? 0 : guid.GetRawValue();
    }

    uint8_t LookupCharacterLevel(uint64_t guid)
    {
        QueryResult result = CharacterDatabase.Query("SELECT level FROM characters WHERE guid = {}", guid);
        return result ? (*result)[0].Get<uint8_t>() : 0;
    }
    bool HandleHearthsideStatus(ChatHandler* handler, Optional<std::string_view>)
    {
        handler->PSendSysMessage("[HearthsideChat] Enable: {}  Debug: {}",
            g_HsEnable ? "on" : "off", g_HsDebugEnabled ? "on" : "off");
        handler->PSendSysMessage("[HearthsideChat] Backend: {}  Queue depth: {}/{}",
            Hs_IsBackendDown() ? "DOWN (circuit breaker open)" : "up",
            Hs_PendingQueueDepth(), g_HsQueueMaxDepth);
        handler->PSendSysMessage("[HearthsideChat] MaxTier - DirectReply: {}  Ambient: {}  Openers: {}  BotToBot: {}  Reflex: {}  EngagementFollowUp: {}",
            g_HsMaxTierDirectReply, g_HsMaxTierAmbient, g_HsMaxTierOpeners, g_HsMaxTierBotToBot, g_HsMaxTierReflex, g_HsMaxTierEngagementFollowUp);
        handler->PSendSysMessage("[HearthsideChat] BotQuestion mode: {}", g_HsBotQuestionMode);
        handler->PSendSysMessage("[HearthsideChat] Generator: {}  Reactive idle: {}  Rows added this session: {}  Rows evicted this session: {}",
            g_HsGeneratorEnabled ? "on" : "off", Hs_IsReactiveIdle() ? "yes" : "no",
            Hs_GeneratorRowsAddedThisSession(), Hs_RowsEvictedThisSession());
        handler->PSendSysMessage("[HearthsideChat] Openers fired this session: {}", Hs_OpenersFiredThisSession());
        handler->PSendSysMessage("[HearthsideChat] Engagement follow-ups fired this session: {}", Hs_EngagementFollowUpsFiredThisSession());
        handler->PSendSysMessage("[HearthsideChat] Script reserve: {}  Active runs: {}",
            Hs_ScriptReserveDepth(), Hs_ActiveScriptRunCount());
        handler->PSendSysMessage("[HearthsideChat] Identity rows: {}  Card-active (ring 3): {}  Promotions this session: {}",
            Hs_IdentityRowCount(), Hs_CardActiveCount(), Hs_PromotionsThisSession());
        handler->PSendSysMessage("[HearthsideChat] Demotions this session: {}  Retirements this session: {}",
            Hs_DemotionsThisSession(), Hs_RetirementsThisSession());
        handler->PSendSysMessage("[HearthsideChat] Memory rows: {}", Hs_MemoryRowCount());
        return true;
    }

    // PLAN.md §4.7 "No automatic harvesting of reactive replies into the
    // corpus (decided)" -- the honest version of the idea: a GM manually
    // promotes the last *pre-style* reply from a named bot into a category,
    // reusing step 12's whole validation gate rather than a new one.
    bool HandleHearthsideCapture(ChatHandler* handler, std::string_view botNameArg, std::string_view categoryArg)
    {
        std::string botName  = std::string(botNameArg);
        std::string category = std::string(categoryArg);

        Player* bot = ObjectAccessor::FindPlayerByName(botName);
        if (!bot || !bot->IsInWorld())
        {
            handler->PSendSysMessage("[HearthsideChat] Bot '{}' not found or not online.", botName);
            return true;
        }
        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!botAI || !botAI->IsBotAI())
        {
            handler->PSendSysMessage("[HearthsideChat] '{}' is not a bot.", botName);
            return true;
        }

        std::string preStyleText = Hs_LastPreStyleReply(botName);
        if (preStyleText.empty())
        {
            handler->PSendSysMessage("[HearthsideChat] No recent reactive-tier reply from '{}' to capture.", botName);
            return true;
        }

        std::string axis = Hs_LookupCategoryAxis(category);
        if (axis.empty())
        {
            handler->PSendSysMessage("[HearthsideChat] Unknown category '{}'.", category);
            return true;
        }

        std::string tagColumn, tagValueSql;
        if (axis == "class")
        {
            tagColumn   = "class_tag";
            tagValueSql = std::to_string(bot->getClass());
        }
        else if (axis == "level_band")
        {
            tagColumn   = "level_band_tag";
            tagValueSql = "'" + Hs_LevelBandFor(bot->GetLevel()) + "'";
        }
        else if (axis != "none")
        {
            handler->PSendSysMessage("[HearthsideChat] Category '{}' uses the '{}' axis, which capture doesn't support yet.", category, axis);
            return true;
        }

        // gm-capture is a one-off GM action, not a versioned generation run
        // -- prompt_version stays NULL (empty string here, coalesced to
        // NULL by Hs_TryInsertCorpusRow), same convention as a hand-authored row.
        HsGenVerdict verdict = Hs_TryInsertCorpusRow(category, tagColumn, tagValueSql, preStyleText, "gm-capture", "");
        if (verdict.accepted)
            handler->PSendSysMessage("[HearthsideChat] Captured into '{}': \"{}\"", category, preStyleText);
        else
            handler->PSendSysMessage("[HearthsideChat] Capture rejected ({}): \"{}\"", verdict.reason, preStyleText);

        return true;
    }

    // §4.19/§7 step 19: "inspect... a bot's ring and card." Ring 3 is
    // reported directly (card_active is bot-global); rings 1/2 are only
    // meaningful relative to one specific player, so this reports the
    // row-level facts rather than a single invented ring number -- see
    // HsIdentityInspection's doc comment in hs_identity_store.h.
    bool HandleHearthsideInspect(ChatHandler* handler, std::string_view botNameArg)
    {
        std::string botName = std::string(botNameArg);
        uint64_t botGuid = ResolveBotGuidByName(botName);
        if (botGuid == 0)
        {
            handler->PSendSysMessage("[HearthsideChat] '{}' not found.", botName);
            return true;
        }

        HsIdentityInspection insp = Hs_InspectIdentity(botGuid);
        if (!insp.hasIdentityRow)
        {
            handler->PSendSysMessage("[HearthsideChat] '{}' has no identity row (never scored a qualifying interaction).", botName);
            return true;
        }

        handler->PSendSysMessage("[HearthsideChat] {} -- archetype: {}  last known level: {}  interaction score: {}",
            botName, insp.archetype, insp.lastKnownLevel, insp.interactionScore);
        handler->PSendSysMessage("[HearthsideChat] Promoted: {}  Card-active: {}  Pinned-by-friend: {}  Has memory rows: {}",
            insp.promoted ? "yes" : "no", insp.cardActive ? "yes" : "no",
            insp.pinnedByFriend ? "yes" : "no", insp.hasAnyMemoryRows ? "yes" : "no");
        if (insp.cardActive)
            handler->PSendSysMessage("[HearthsideChat] Voice: \"{}\"", insp.voiceBlock);
        return true;
    }

    // §4.19/§7 step 19's "review commands" -- the read counterpart to
    // evict-run: eyeball a category's (optionally one run's) actual output
    // before deciding whether to evict it. PLAN.md names "review commands"
    // once with no further detail; this is the documented interpretation,
    // not an assumed one (hs_generator.h's Hs_ReviewCorpusRows).
    bool HandleHearthsideReview(ChatHandler* handler, std::string_view categoryArg, Optional<std::string_view> promptVersionArg)
    {
        std::string category      = std::string(categoryArg);
        std::string promptVersion = promptVersionArg ? std::string(*promptVersionArg) : "";

        std::vector<HsCorpusReviewRow> rows = Hs_ReviewCorpusRows(category, promptVersion, 15);
        if (rows.empty())
        {
            handler->PSendSysMessage("[HearthsideChat] No rows found for '{}'.", category);
            return true;
        }

        handler->PSendSysMessage("[HearthsideChat] {} row(s) for '{}' (most recent first):", rows.size(), category);
        for (auto const& row : rows)
            handler->PSendSysMessage("  [{}x used, {}] \"{}\"", row.timesUsed,
                row.model.empty() ? "hand-authored" : row.model, row.text);
        return true;
    }

    // §4.19/§7 step 19: "inspect/force a bot's ring and card" -- the force
    // half. Forces promotion (ring 3 eligibility) without waiting on the
    // score threshold; the generator's existing pickup still does the
    // actual card generation, so this doesn't block the world thread on an
    // LLM call.
    bool HandleHearthsidePromote(ChatHandler* handler, std::string_view botNameArg)
    {
        std::string botName = std::string(botNameArg);
        uint64_t botGuid = ResolveBotGuidByName(botName);
        uint8_t  level   = botGuid ? LookupCharacterLevel(botGuid) : 0;
        if (botGuid == 0 || level == 0)
        {
            handler->PSendSysMessage("[HearthsideChat] '{}' not found.", botName);
            return true;
        }

        bool promoted = Hs_ForcePromote(botGuid, level);
        handler->PSendSysMessage(promoted
            ? "[HearthsideChat] '{}' promoted -- the generator will pick up its card on its next idle cycle."
            : "[HearthsideChat] '{}' was already promoted.", botName);
        return true;
    }

    // Force demotion on demand -- same action Hs_RunIdentityDailySweep's
    // dormancy check takes automatically, without waiting kHsCardDormancyDays.
    bool HandleHearthsideDemote(ChatHandler* handler, std::string_view botNameArg)
    {
        std::string botName = std::string(botNameArg);
        uint64_t botGuid = ResolveBotGuidByName(botName);
        if (botGuid == 0)
        {
            handler->PSendSysMessage("[HearthsideChat] '{}' not found.", botName);
            return true;
        }

        bool demoted = Hs_ForceDemote(botGuid);
        handler->PSendSysMessage(demoted
            ? "[HearthsideChat] '{}' demoted -- card text retained, card_active cleared."
            : "[HearthsideChat] '{}' had no active card to demote.", botName);
        return true;
    }

    // §4.19/§7 step 19: "force retirement." Reuses step 17's Hs_RetireCard
    // directly against the bot's current DB level (works whether or not the
    // bot is currently online, unlike `.hearthside capture` above).
    bool HandleHearthsideRetire(ChatHandler* handler, std::string_view botNameArg)
    {
        std::string botName = std::string(botNameArg);
        uint64_t botGuid = ResolveBotGuidByName(botName);
        uint8_t  level   = botGuid ? LookupCharacterLevel(botGuid) : 0;
        if (botGuid == 0 || level == 0)
        {
            handler->PSendSysMessage("[HearthsideChat] '{}' not found.", botName);
            return true;
        }

        Hs_RetireCard(botGuid, level);
        handler->PSendSysMessage("[HearthsideChat] '{}' retired -- card cleared, memory dropped, returned to ring 0.", botName);
        return true;
    }

    // §4.19/§7 step 19: "pin a bot into the exclude list regardless of
    // card_active." A raw exclude-vector push, independent of the card/
    // pinned_by_friend machinery -- the bot doesn't need an hside_identity
    // row at all.
    bool HandleHearthsidePin(ChatHandler* handler, std::string_view botNameArg)
    {
        std::string botName = std::string(botNameArg);
        uint64_t botGuid = ResolveBotGuidByName(botName);
        if (botGuid == 0)
        {
            handler->PSendSysMessage("[HearthsideChat] '{}' not found.", botName);
            return true;
        }

        Hs_GmPinBot(botGuid);
        handler->PSendSysMessage("[HearthsideChat] '{}' pinned -- excluded from mod-playerbots' recycler regardless of card state.", botName);
        return true;
    }

    bool HandleHearthsideUnpin(ChatHandler* handler, std::string_view botNameArg)
    {
        std::string botName = std::string(botNameArg);
        uint64_t botGuid = ResolveBotGuidByName(botName);
        if (botGuid == 0)
        {
            handler->PSendSysMessage("[HearthsideChat] '{}' not found.", botName);
            return true;
        }

        Hs_GmUnpinBot(botGuid);
        handler->PSendSysMessage("[HearthsideChat] '{}' unpinned -- eligible for recycling again (unless still card_active).", botName);
        return true;
    }

    // §4.19/§7 step 19: "bulk-evict by generation run." A run is identified
    // by its prompt_version tag (§4.4).
    bool HandleHearthsideEvictRun(ChatHandler* handler, std::string_view promptVersionArg)
    {
        std::string promptVersion = std::string(promptVersionArg);
        uint32_t evicted = Hs_EvictGenerationRun(promptVersion);
        handler->PSendSysMessage("[HearthsideChat] Evicted {} row(s) tagged prompt_version '{}'.", evicted, promptVersion);
        return true;
    }
}

HsCommandScript::HsCommandScript() : CommandScript("HsCommandScript") {}

ChatCommandTable HsCommandScript::GetCommands() const
{
    static ChatCommandTable hearthsideSubCommands =
    {
        { "status",     HandleHearthsideStatus,    SEC_GAMEMASTER, Console::Yes },
        { "capture",    HandleHearthsideCapture,   SEC_GAMEMASTER, Console::Yes },
        { "inspect",    HandleHearthsideInspect,   SEC_GAMEMASTER, Console::Yes },
        { "review",     HandleHearthsideReview,    SEC_GAMEMASTER, Console::Yes },
        { "promote",    HandleHearthsidePromote,   SEC_GAMEMASTER, Console::Yes },
        { "demote",     HandleHearthsideDemote,    SEC_GAMEMASTER, Console::Yes },
        { "retire",     HandleHearthsideRetire,    SEC_GAMEMASTER, Console::Yes },
        { "pin",        HandleHearthsidePin,       SEC_GAMEMASTER, Console::Yes },
        { "unpin",      HandleHearthsideUnpin,     SEC_GAMEMASTER, Console::Yes },
        { "evict-run",  HandleHearthsideEvictRun,  SEC_GAMEMASTER, Console::Yes },
    };

    static ChatCommandTable rootTable =
    {
        { "hearthside", hearthsideSubCommands },
    };

    return rootTable;
}
