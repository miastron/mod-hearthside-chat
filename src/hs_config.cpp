#include "hs_config.h"
#include "Config.h"
#include "hs_channel.h"

#include <set>
#include <sstream>

bool g_HsEnable       = true;
bool g_HsDebugEnabled = false;
bool g_HsDebugChatLogEnabled = false;

std::string g_HsLLMApiType = "llamacpp";
std::string g_HsLLMUrl     = "http://127.0.0.1:8080";
std::string g_HsLLMModel   = "";
std::string g_HsLLMApiKey  = "";
uint32_t     g_HsLLMTimeoutSeconds = 20;
uint32_t     g_HsLLMMaxTokens      = 60;

// A "player, not character" frame -- avoids the terse "Hmph."-collapse
// failure mode seen with an in-fiction frame. This is a placeholder
// single-sentence frame; the fuller shared-prefix baseline persona +
// few-shot register examples live elsewhere.
std::string g_HsLLMSystemPrompt =
    "You are an ordinary player in World of Warcraft: Wrath of the Lich King, chatting in game. "
    "Reply the way a real player types in chat: casual, brief, one short line. "
    "No roleplay narration, no asterisks, no fantasy dialogue, no mention of being an AI or a game.";

// Four lines (2 trigger/reply pairs) per bot-player pair by default;
// live-tunable without a rebuild.
uint32_t g_HsLLMHistoryTurns  = 2;
float     g_HsLLMDryMultiplier = 0.0f;

float     g_HsSayDistance             = 30.0f;
uint32_t  g_HsReplyChanceWhisper      = 100;
bool      g_HsDisableRepliesInCombat  = true;

std::string g_HsExcludeNames = "";

namespace
{
    std::set<std::string> g_ExcludeNameSet;

    std::string Trim(const std::string& s)
    {
        size_t begin = s.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos)
            return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(begin, end - begin + 1);
    }

    void RebuildExcludeNameSet()
    {
        g_ExcludeNameSet.clear();
        std::stringstream ss(g_HsExcludeNames);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            std::string trimmed = Trim(token);
            if (!trimmed.empty())
                g_ExcludeNameSet.insert(trimmed);
        }
    }
}

bool Hs_IsExcludedBotName(const std::string& botName)
{
    return g_ExcludeNameSet.count(botName) != 0;
}

// Measured warm serial reply ceiling is 104/min. Default bucket rate sits a
// little under that so the bucket binds before the backend itself would
// queue up.
uint32_t g_HsQueueTTLSeconds             = 15;
uint32_t g_HsQueueMaxDepth               = 20;
uint32_t g_HsBucketRepliesPerMinute      = 90;
uint32_t g_HsBucketBurstCapacity         = 8;
uint32_t g_HsBotCooldownSeconds          = 8;
uint32_t g_HsBreakerFailureThreshold     = 3;
uint32_t g_HsBreakerProbeIntervalSeconds = 15;

bool     g_HsTypingDelayEnabled   = true;
uint32_t g_HsTypingDelayMaxMs     = 6000;
uint32_t g_HsMinDeliveryDelayMs   = 400;

std::string g_HsMaxTierDirectReply = "inference";
std::string g_HsMaxTierAmbient     = "corpus";
std::string g_HsMaxTierOpeners     = "corpus";
std::string g_HsMaxTierBotToBot    = "corpus";
std::string g_HsMaxTierReflex      = "reflex";
std::string g_HsMaxTierEngagementFollowUp = "off";

// Trade/General/World are on by default at a deliberately low starting
// rate (§4.17's "a too-quiet channel is recoverable, a too-noisy one has
// already cost the illusion"); the remaining four default off, matching
// PLAN.md §4.17's table, but stay operator-adjustable via config like every
// other MaxTier.* key rather than a hardcoded block.
std::string g_HsChannelTradeMaxTier            = "corpus";
uint32_t     g_HsChannelTradeRatePerMin         = 3;
uint32_t     g_HsChannelTradeMaxCandidates      = 8;

std::string g_HsChannelGeneralMaxTier          = "corpus";
uint32_t     g_HsChannelGeneralRatePerMin       = 3;
uint32_t     g_HsChannelGeneralMaxCandidates    = 8;

std::string g_HsChannelWorldMaxTier            = "corpus";
uint32_t     g_HsChannelWorldRatePerMin         = 3;
uint32_t     g_HsChannelWorldMaxCandidates      = 8;

std::string g_HsChannelLookingForGroupMaxTier         = "off";
uint32_t     g_HsChannelLookingForGroupRatePerMin      = 0;
uint32_t     g_HsChannelLookingForGroupMaxCandidates   = 0;

std::string g_HsChannelGuildRecruitmentMaxTier        = "off";
uint32_t     g_HsChannelGuildRecruitmentRatePerMin     = 0;
uint32_t     g_HsChannelGuildRecruitmentMaxCandidates  = 0;

std::string g_HsChannelLocalDefenseMaxTier     = "off";
uint32_t     g_HsChannelLocalDefenseRatePerMin  = 0;
uint32_t     g_HsChannelLocalDefenseMaxCandidates = 0;

std::string g_HsChannelWorldDefenseMaxTier     = "off";
uint32_t     g_HsChannelWorldDefenseRatePerMin  = 0;
uint32_t     g_HsChannelWorldDefenseMaxCandidates = 0;

std::string g_HsBotQuestionMode = "wink";

bool     g_HsGroundedAnswersEnabled  = true;
uint32_t g_HsGroundedFuzzyMaxDistance = 2;

bool        g_HsGeneratorEnabled              = false;
std::string g_HsGeneratorLLMApiType           = "llamacpp";
std::string g_HsGeneratorLLMUrl               = "http://127.0.0.1:8080";
std::string g_HsGeneratorLLMModel             = "";
std::string g_HsGeneratorLLMApiKey            = "";
uint32_t     g_HsGeneratorLLMTimeoutSeconds    = 30;
uint32_t     g_HsGeneratorLLMMaxTokens         = 60;
uint32_t     g_HsGeneratorRowsPerBucket        = 20;
uint32_t     g_HsGeneratorPollIntervalSeconds  = 5;
uint32_t     g_HsGeneratorQuotaSatisfiedBackoffSeconds = 300;
std::string g_HsGeneratorPromptVersion        = "v1";
uint32_t     g_HsGeneratorScriptReserveTarget = 15;

uint32_t     g_HsHttpServerPort           = 0;
std::string g_HsHttpServerBind            = "127.0.0.1";
std::string g_HsHttpServerPrivateKey      = "";
uint32_t     g_HsHttpServerTimeoutSeconds = 10;
bool         g_HsHttpControlEnable        = false;

void LoadHearthsideChatConfig()
{
    g_HsEnable       = sConfigMgr->GetOption<bool>("HearthsideChat.Enable", true);
    g_HsDebugEnabled = sConfigMgr->GetOption<bool>("HearthsideChat.DebugEnabled", false);

    g_HsLLMApiType         = sConfigMgr->GetOption<std::string>("HearthsideChat.LLM.ApiType", "llamacpp");
    g_HsLLMUrl              = sConfigMgr->GetOption<std::string>("HearthsideChat.LLM.Url", "http://127.0.0.1:8080");
    g_HsLLMModel            = sConfigMgr->GetOption<std::string>("HearthsideChat.LLM.Model", "");
    g_HsLLMApiKey           = sConfigMgr->GetOption<std::string>("HearthsideChat.LLM.ApiKey", "");
    g_HsLLMTimeoutSeconds   = sConfigMgr->GetOption<uint32_t>("HearthsideChat.LLM.TimeoutSeconds", 20);
    g_HsLLMMaxTokens        = sConfigMgr->GetOption<uint32_t>("HearthsideChat.LLM.MaxTokens", 60);
    g_HsLLMSystemPrompt     = sConfigMgr->GetOption<std::string>("HearthsideChat.LLM.SystemPrompt", g_HsLLMSystemPrompt);
    g_HsLLMHistoryTurns     = sConfigMgr->GetOption<uint32_t>("HearthsideChat.LLM.HistoryTurns", 2);
    g_HsLLMDryMultiplier    = sConfigMgr->GetOption<float>("HearthsideChat.LLM.DryMultiplier", 0.0f);

    g_HsSayDistance            = sConfigMgr->GetOption<float>("HearthsideChat.Say.Distance", 30.0f);
    g_HsReplyChanceWhisper     = sConfigMgr->GetOption<uint32_t>("HearthsideChat.ReplyChance.Whisper", 100);
    g_HsDisableRepliesInCombat = sConfigMgr->GetOption<bool>("HearthsideChat.DisableRepliesInCombat", true);

    g_HsExcludeNames = sConfigMgr->GetOption<std::string>("HearthsideChat.ExcludeNames", "");
    RebuildExcludeNameSet();

    g_HsDebugChatLogEnabled = sConfigMgr->GetOption<bool>("HearthsideChat.DebugChatLog.Enable", false);

    g_HsQueueTTLSeconds             = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Queue.TTLSeconds", 15);
    g_HsQueueMaxDepth               = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Queue.MaxDepth", 20);
    g_HsBucketRepliesPerMinute      = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Bucket.RepliesPerMinute", 90);
    g_HsBucketBurstCapacity         = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Bucket.BurstCapacity", 8);
    g_HsBotCooldownSeconds          = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Bot.CooldownSeconds", 8);
    g_HsBreakerFailureThreshold     = sConfigMgr->GetOption<uint32_t>("HearthsideChat.CircuitBreaker.FailureThreshold", 3);
    g_HsBreakerProbeIntervalSeconds = sConfigMgr->GetOption<uint32_t>("HearthsideChat.CircuitBreaker.ProbeIntervalSeconds", 15);

    g_HsTypingDelayEnabled   = sConfigMgr->GetOption<bool>("HearthsideChat.TypingDelay.Enable", true);
    g_HsTypingDelayMaxMs     = sConfigMgr->GetOption<uint32_t>("HearthsideChat.TypingDelay.MaxMs", 6000);
    g_HsMinDeliveryDelayMs   = sConfigMgr->GetOption<uint32_t>("HearthsideChat.MinDeliveryDelayMs", 400);

    g_HsMaxTierDirectReply = sConfigMgr->GetOption<std::string>("HearthsideChat.MaxTier.DirectReply", "inference");
    g_HsMaxTierAmbient     = sConfigMgr->GetOption<std::string>("HearthsideChat.MaxTier.Ambient", "corpus");
    g_HsMaxTierOpeners     = sConfigMgr->GetOption<std::string>("HearthsideChat.MaxTier.Openers", "corpus");
    g_HsMaxTierBotToBot    = sConfigMgr->GetOption<std::string>("HearthsideChat.MaxTier.BotToBot", "corpus");
    g_HsMaxTierReflex      = sConfigMgr->GetOption<std::string>("HearthsideChat.MaxTier.Reflex", "reflex");
    g_HsMaxTierEngagementFollowUp = sConfigMgr->GetOption<std::string>("HearthsideChat.MaxTier.EngagementFollowUp", "off");

    g_HsChannelTradeMaxTier            = sConfigMgr->GetOption<std::string>("HearthsideChat.Channel.Trade.MaxTier", "corpus");
    g_HsChannelTradeRatePerMin          = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Channel.Trade.RatePerMin", 3);
    g_HsChannelTradeMaxCandidates        = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Channel.Trade.MaxCandidates", 8);

    g_HsChannelGeneralMaxTier           = sConfigMgr->GetOption<std::string>("HearthsideChat.Channel.General.MaxTier", "corpus");
    g_HsChannelGeneralRatePerMin         = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Channel.General.RatePerMin", 3);
    g_HsChannelGeneralMaxCandidates       = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Channel.General.MaxCandidates", 8);

    g_HsChannelWorldMaxTier             = sConfigMgr->GetOption<std::string>("HearthsideChat.Channel.World.MaxTier", "corpus");
    g_HsChannelWorldRatePerMin           = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Channel.World.RatePerMin", 3);
    g_HsChannelWorldMaxCandidates         = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Channel.World.MaxCandidates", 8);

    g_HsChannelLookingForGroupMaxTier          = sConfigMgr->GetOption<std::string>("HearthsideChat.Channel.LookingForGroup.MaxTier", "off");
    g_HsChannelLookingForGroupRatePerMin        = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Channel.LookingForGroup.RatePerMin", 0);
    g_HsChannelLookingForGroupMaxCandidates      = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Channel.LookingForGroup.MaxCandidates", 0);

    g_HsChannelGuildRecruitmentMaxTier         = sConfigMgr->GetOption<std::string>("HearthsideChat.Channel.GuildRecruitment.MaxTier", "off");
    g_HsChannelGuildRecruitmentRatePerMin       = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Channel.GuildRecruitment.RatePerMin", 0);
    g_HsChannelGuildRecruitmentMaxCandidates     = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Channel.GuildRecruitment.MaxCandidates", 0);

    g_HsChannelLocalDefenseMaxTier      = sConfigMgr->GetOption<std::string>("HearthsideChat.Channel.LocalDefense.MaxTier", "off");
    g_HsChannelLocalDefenseRatePerMin    = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Channel.LocalDefense.RatePerMin", 0);
    g_HsChannelLocalDefenseMaxCandidates  = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Channel.LocalDefense.MaxCandidates", 0);

    g_HsChannelWorldDefenseMaxTier      = sConfigMgr->GetOption<std::string>("HearthsideChat.Channel.WorldDefense.MaxTier", "off");
    g_HsChannelWorldDefenseRatePerMin    = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Channel.WorldDefense.RatePerMin", 0);
    g_HsChannelWorldDefenseMaxCandidates  = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Channel.WorldDefense.MaxCandidates", 0);

    {
        HsChannelPolicy table[kHsChannelKindCount] = {};
        table[static_cast<size_t>(HsChannelKind::Trade)] =
            { HsParseTier(g_HsChannelTradeMaxTier), g_HsChannelTradeRatePerMin, g_HsChannelTradeMaxCandidates };
        table[static_cast<size_t>(HsChannelKind::General)] =
            { HsParseTier(g_HsChannelGeneralMaxTier), g_HsChannelGeneralRatePerMin, g_HsChannelGeneralMaxCandidates };
        table[static_cast<size_t>(HsChannelKind::World)] =
            { HsParseTier(g_HsChannelWorldMaxTier), g_HsChannelWorldRatePerMin, g_HsChannelWorldMaxCandidates };
        table[static_cast<size_t>(HsChannelKind::LookingForGroup)] =
            { HsParseTier(g_HsChannelLookingForGroupMaxTier), g_HsChannelLookingForGroupRatePerMin, g_HsChannelLookingForGroupMaxCandidates };
        table[static_cast<size_t>(HsChannelKind::GuildRecruitment)] =
            { HsParseTier(g_HsChannelGuildRecruitmentMaxTier), g_HsChannelGuildRecruitmentRatePerMin, g_HsChannelGuildRecruitmentMaxCandidates };
        table[static_cast<size_t>(HsChannelKind::LocalDefense)] =
            { HsParseTier(g_HsChannelLocalDefenseMaxTier), g_HsChannelLocalDefenseRatePerMin, g_HsChannelLocalDefenseMaxCandidates };
        table[static_cast<size_t>(HsChannelKind::WorldDefense)] =
            { HsParseTier(g_HsChannelWorldDefenseMaxTier), g_HsChannelWorldDefenseRatePerMin, g_HsChannelWorldDefenseMaxCandidates };
        Hs_SetChannelPolicyTable(table);
    }

    g_HsBotQuestionMode = sConfigMgr->GetOption<std::string>("HearthsideChat.BotQuestion", "wink");

    g_HsGroundedAnswersEnabled  = sConfigMgr->GetOption<bool>("HearthsideChat.GroundedAnswers", true);
    g_HsGroundedFuzzyMaxDistance = sConfigMgr->GetOption<uint32_t>("HearthsideChat.GroundedAnswers.FuzzyMaxDistance", 2);

    g_HsGeneratorEnabled                    = sConfigMgr->GetOption<bool>("HearthsideChat.Generator.Enable", false);
    g_HsGeneratorLLMApiType                 = sConfigMgr->GetOption<std::string>("HearthsideChat.Generator.LLM.ApiType", "llamacpp");
    g_HsGeneratorLLMUrl                      = sConfigMgr->GetOption<std::string>("HearthsideChat.Generator.LLM.Url", "http://127.0.0.1:8080");
    g_HsGeneratorLLMModel                    = sConfigMgr->GetOption<std::string>("HearthsideChat.Generator.LLM.Model", "");
    g_HsGeneratorLLMApiKey                   = sConfigMgr->GetOption<std::string>("HearthsideChat.Generator.LLM.ApiKey", "");
    g_HsGeneratorLLMTimeoutSeconds           = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Generator.LLM.TimeoutSeconds", 30);
    g_HsGeneratorLLMMaxTokens                = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Generator.LLM.MaxTokens", 60);
    g_HsGeneratorRowsPerBucket               = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Generator.RowsPerBucket", 20);
    g_HsGeneratorPollIntervalSeconds         = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Generator.PollIntervalSeconds", 5);
    g_HsGeneratorQuotaSatisfiedBackoffSeconds = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Generator.QuotaSatisfiedBackoffSeconds", 300);
    g_HsGeneratorPromptVersion               = sConfigMgr->GetOption<std::string>("HearthsideChat.Generator.PromptVersion", "v1");
    g_HsGeneratorScriptReserveTarget          = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Generator.ScriptReserveTarget", 15);

    g_HsHttpServerPort           = sConfigMgr->GetOption<uint32_t>("HearthsideChat.HttpServerPort", 0);
    g_HsHttpServerBind            = sConfigMgr->GetOption<std::string>("HearthsideChat.HttpServerBind", "127.0.0.1");
    g_HsHttpServerPrivateKey      = sConfigMgr->GetOption<std::string>("HearthsideChat.HttpServerPrivateKey", "");
    g_HsHttpServerTimeoutSeconds = sConfigMgr->GetOption<uint32_t>("HearthsideChat.HttpServerTimeoutSeconds", 10);
    g_HsHttpControlEnable         = sConfigMgr->GetOption<bool>("HearthsideChat.HttpControlEnable", false);
}

HsConfigWorldScript::HsConfigWorldScript() : WorldScript("HsConfigWorldScript") { }

void HsConfigWorldScript::OnStartup()
{
    LoadHearthsideChatConfig();
}

void HsConfigWorldScript::OnAfterConfigLoad(bool reload)
{
    if (reload)
        LoadHearthsideChatConfig();
}
