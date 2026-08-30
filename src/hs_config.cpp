#include "hs_config.h"
#include "Config.h"
#include "Log.h"
#include "hs_channel.h"

#include <mutex>
#include <set>
#include <sstream>

bool g_HsEnable       = true;
bool g_HsDebugEnabled = false;
bool g_HsBridgeEnable = true;
bool g_HsDebugChatLogEnabled = false;

std::string g_HsLLMApiType  = "llamacpp";
std::string g_HsLLMUrl      = "http://127.0.0.1:8080";
std::string g_HsLLMModel    = "";
std::string g_HsLLMApiKey   = "";
uint32_t     g_HsLLMTimeoutSeconds = 20;
uint32_t     g_HsLLMMaxTokens      = 60;
std::string g_HsLLMTemplate = "llama3";

// A "player, not character" frame, avoids the terse "Hmph."-collapse
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

uint32_t  g_HsReplyCountZeroPercent   = 30;
uint32_t  g_HsReplyCountOnePercent    = 60;
uint32_t  g_HsReplyCountTwoPercent    = 10;

std::string g_HsExcludeNames = "";

namespace
{
    // Guards every std::string global in this file against the `.reload
    // config` rewrite. See hs_config.h's "Reading the std::string globals
    // from a thread that is not the world thread" for why a stale read is
    // not the failure mode and a freed buffer is. Held for the whole of
    // LoadHearthsideChatConfig (which only touches sConfigMgr and these
    // globals, so it is microseconds and cannot re-enter this lock), and
    // released per-copy by the three accessors below.
    std::mutex g_ConfigStringMutex;

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

bool     g_HsDistractedEnabled         = true;
uint32_t g_HsDistractedMinDelaySeconds = 25;
uint32_t g_HsDistractedMaxDelaySeconds = 60;
uint32_t g_HsDistractedCooldownSeconds = 600;

std::string g_HsMaxTierDirectReply = "inference";
std::string g_HsMaxTierAmbient     = "corpus";
std::string g_HsMaxTierOpeners     = "corpus";
std::string g_HsMaxTierBotToBot    = "corpus";
std::string g_HsMaxTierReflex      = "reflex";
std::string g_HsMaxTierEngagementFollowUp = "off";
std::string g_HsMaxTierEvents             = "inference";

// One event roughly every four seconds sustained, with room for a wipe to
// spend four at once. Starting guesses shaped against the reply bucket's
// 90/min, not measurements.
uint32_t g_HsEventBucketRepliesPerMinute = 15;
uint32_t g_HsEventBucketBurstCapacity    = 4;

// Deliberately under the event bucket's 15/min: this budget is shared by all
// three unprompted-speech producers (ambient, openers, scripted scenes), and
// unlike an event reaction none of them fires on anything that happened. Six
// a minute realm-wide is roughly one unprompted line every ten seconds
// somewhere on the realm, with two bankable for a burst. Starting guesses,
// not measurements. Per hs_config.h's asymmetry note, the direction to
// tune this from live evidence is up, never down.
uint32_t g_HsAmbientBucketRepliesPerMinute = 6;
uint32_t g_HsAmbientBucketBurstCapacity    = 2;

// 30 minutes: a given bot speaks unprompted at most twice an hour. Much
// longer than the opener's own 10-minute per-pair cooldown, because that one
// is scoped to a pair and this one is scoped to the bot across every player
// who might hear it.
uint32_t g_HsAmbientBotCooldownSeconds = 1800;
bool     g_HsAmbientRequireRealPlayer  = true;

bool g_HsAmbientSayEnable   = true;
bool g_HsAmbientPartyEnable = true;
bool g_HsAmbientRaidEnable  = true;

uint32_t g_HsAmbientSayFireChancePercent      = 60;
uint32_t g_HsOpenerFireChancePercent          = 60;
uint32_t g_HsScriptProximityFireChancePercent = 20;

// Starting guesses, not measurements, the same footing hs_script.cpp's
// kScanFireChancePercent is on. 55% decaying at 60% gives roughly a 1.4-hop
// average chain (0.55 + 0.55*0.33 + 0.55*0.20 over the three permitted
// depths), which is a short exchange rather than a monologue.
uint32_t g_HsBotChainMaxDepth             = 3;
uint32_t g_HsBotChainBaseChancePercent    = 55;
uint32_t g_HsBotChainDecayPercent         = 60;
uint32_t g_HsBotChainScopeCooldownSeconds = 180;
bool     g_HsBotChainRequireRealPlayer    = true;

// Trade and General are on by default at a deliberately low starting
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
std::string g_HsGeneratorLLMTemplate           = "llama3";
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

HsLLMStrings Hs_LLMStringsSnapshot()
{
    std::lock_guard<std::mutex> lock(g_ConfigStringMutex);
    return { g_HsLLMApiType, g_HsLLMUrl, g_HsLLMModel,
             g_HsLLMApiKey, g_HsLLMTemplate, g_HsLLMSystemPrompt };
}

HsGeneratorStrings Hs_GeneratorStringsSnapshot()
{
    std::lock_guard<std::mutex> lock(g_ConfigStringMutex);
    return { g_HsGeneratorLLMApiType, g_HsGeneratorLLMUrl, g_HsGeneratorLLMModel,
             g_HsGeneratorLLMApiKey, g_HsGeneratorLLMTemplate, g_HsGeneratorPromptVersion };
}

std::string Hs_ConfigString(const std::string& configGlobal)
{
    // The reference parameter is the point: the copy happens inside the lock,
    // so the caller never holds a reference into a global the world thread
    // may reassign. Passing anything other than a g_Hs* string global here is
    // harmless but pointless.
    std::lock_guard<std::mutex> lock(g_ConfigStringMutex);
    return configGlobal;
}

void LoadHearthsideChatConfig()
{
    // One lock for the whole load rather than a block per string group: the
    // body is a straight run of sConfigMgr reads and global assignments with
    // no callback that could re-enter this mutex, so the exclusion window is
    // microseconds and every string is covered without a reader being able to
    // observe a half-applied reload.
    std::lock_guard<std::mutex> lock(g_ConfigStringMutex);

    g_HsEnable       = sConfigMgr->GetOption<bool>("HearthsideChat.Enable", true);
    g_HsDebugEnabled = sConfigMgr->GetOption<bool>("HearthsideChat.DebugEnabled", false);
    g_HsBridgeEnable = sConfigMgr->GetOption<bool>("HearthsideChat.Bridge.Enable", true);

    g_HsLLMApiType         = sConfigMgr->GetOption<std::string>("HearthsideChat.LLM.ApiType", "llamacpp");
    g_HsLLMUrl              = sConfigMgr->GetOption<std::string>("HearthsideChat.LLM.Url", "http://127.0.0.1:8080");
    g_HsLLMModel            = sConfigMgr->GetOption<std::string>("HearthsideChat.LLM.Model", "");
    g_HsLLMApiKey           = sConfigMgr->GetOption<std::string>("HearthsideChat.LLM.ApiKey", "");
    g_HsLLMTimeoutSeconds   = sConfigMgr->GetOption<uint32_t>("HearthsideChat.LLM.TimeoutSeconds", 20);
    g_HsLLMMaxTokens        = sConfigMgr->GetOption<uint32_t>("HearthsideChat.LLM.MaxTokens", 60);
    g_HsLLMTemplate         = sConfigMgr->GetOption<std::string>("HearthsideChat.LLM.Template", "llama3");
    g_HsLLMSystemPrompt     = sConfigMgr->GetOption<std::string>("HearthsideChat.LLM.SystemPrompt", g_HsLLMSystemPrompt);
    g_HsLLMHistoryTurns     = sConfigMgr->GetOption<uint32_t>("HearthsideChat.LLM.HistoryTurns", 2);
    g_HsLLMDryMultiplier    = sConfigMgr->GetOption<float>("HearthsideChat.LLM.DryMultiplier", 0.0f);

    g_HsSayDistance            = sConfigMgr->GetOption<float>("HearthsideChat.Say.Distance", 20.0f);
    g_HsReplyChanceWhisper     = sConfigMgr->GetOption<uint32_t>("HearthsideChat.ReplyChance.Whisper", 100);
    g_HsDisableRepliesInCombat = sConfigMgr->GetOption<bool>("HearthsideChat.DisableRepliesInCombat", true);

    g_HsReplyCountZeroPercent = sConfigMgr->GetOption<uint32_t>("HearthsideChat.ReplyCount.ZeroPercent", 30);
    g_HsReplyCountOnePercent  = sConfigMgr->GetOption<uint32_t>("HearthsideChat.ReplyCount.OnePercent", 60);
    g_HsReplyCountTwoPercent  = sConfigMgr->GetOption<uint32_t>("HearthsideChat.ReplyCount.TwoPercent", 10);

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

    g_HsDistractedEnabled         = sConfigMgr->GetOption<bool>("HearthsideChat.Distracted.Enable", true);
    g_HsDistractedMinDelaySeconds = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Distracted.MinDelaySeconds", 25);
    g_HsDistractedMaxDelaySeconds = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Distracted.MaxDelaySeconds", 60);
    g_HsDistractedCooldownSeconds = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Distracted.CooldownSeconds", 600);

    // Normalized here rather than at the draw site: hs_queue.cpp feeds these
    // straight to urand(), whose underlying uniform_int_distribution is
    // undefined when min > max. An inverted pair in a hand-edited conf
    // would be a crash, not a misbehavior.
    if (g_HsDistractedMinDelaySeconds > g_HsDistractedMaxDelaySeconds)
    {
        LOG_ERROR("server.loading",
            "[HearthsideChat] Distracted.MinDelaySeconds ({}) exceeds MaxDelaySeconds ({}) -- clamping max up to min.",
            g_HsDistractedMinDelaySeconds, g_HsDistractedMaxDelaySeconds);
        g_HsDistractedMaxDelaySeconds = g_HsDistractedMinDelaySeconds;
    }

    g_HsMaxTierDirectReply = sConfigMgr->GetOption<std::string>("HearthsideChat.MaxTier.DirectReply", "inference");
    g_HsMaxTierAmbient     = sConfigMgr->GetOption<std::string>("HearthsideChat.MaxTier.Ambient", "corpus");
    g_HsMaxTierOpeners     = sConfigMgr->GetOption<std::string>("HearthsideChat.MaxTier.Openers", "corpus");
    g_HsMaxTierBotToBot    = sConfigMgr->GetOption<std::string>("HearthsideChat.MaxTier.BotToBot", "corpus");
    g_HsMaxTierReflex      = sConfigMgr->GetOption<std::string>("HearthsideChat.MaxTier.Reflex", "reflex");
    g_HsMaxTierEngagementFollowUp = sConfigMgr->GetOption<std::string>("HearthsideChat.MaxTier.EngagementFollowUp", "off");
    g_HsMaxTierEvents             = sConfigMgr->GetOption<std::string>("HearthsideChat.MaxTier.Events", "inference");

    g_HsEventBucketRepliesPerMinute = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Events.Bucket.RepliesPerMinute", 15);
    g_HsEventBucketBurstCapacity    = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Events.Bucket.BurstCapacity", 4);

    g_HsAmbientBucketRepliesPerMinute = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Ambient.Bucket.RepliesPerMinute", 6);
    g_HsAmbientBucketBurstCapacity    = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Ambient.Bucket.BurstCapacity", 2);
    g_HsAmbientBotCooldownSeconds     = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Ambient.BotCooldownSeconds", 1800);
    g_HsAmbientRequireRealPlayer      = sConfigMgr->GetOption<bool>("HearthsideChat.Ambient.RequireRealPlayer", true);
    g_HsAmbientSayEnable              = sConfigMgr->GetOption<bool>("HearthsideChat.Ambient.Say.Enable", true);
    g_HsAmbientPartyEnable            = sConfigMgr->GetOption<bool>("HearthsideChat.Ambient.Party.Enable", true);
    g_HsAmbientRaidEnable             = sConfigMgr->GetOption<bool>("HearthsideChat.Ambient.Raid.Enable", true);

    g_HsAmbientSayFireChancePercent      = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Ambient.Say.FireChancePercent", 60);
    g_HsOpenerFireChancePercent          = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Openers.FireChancePercent", 60);
    g_HsScriptProximityFireChancePercent = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Script.Proximity.FireChancePercent", 20);

    g_HsBotChainMaxDepth             = sConfigMgr->GetOption<uint32_t>("HearthsideChat.BotChain.MaxDepth", 3);
    g_HsBotChainBaseChancePercent    = sConfigMgr->GetOption<uint32_t>("HearthsideChat.BotChain.BaseChancePercent", 55);
    g_HsBotChainDecayPercent         = sConfigMgr->GetOption<uint32_t>("HearthsideChat.BotChain.DecayPercent", 60);
    g_HsBotChainScopeCooldownSeconds = sConfigMgr->GetOption<uint32_t>("HearthsideChat.BotChain.ScopeCooldownSeconds", 180);
    g_HsBotChainRequireRealPlayer    = sConfigMgr->GetOption<bool>("HearthsideChat.BotChain.RequireRealPlayer", true);

    // A decay at or above 100% would flatten the taper into a flat chance (or
    // grow it), turning MaxDepth from a backstop into the only thing ending a
    // chain. Clamped rather than honoured, since that is never what an
    // operator raising this key means.
    if (g_HsBotChainDecayPercent > 99)
        g_HsBotChainDecayPercent = 99;

    g_HsChannelTradeMaxTier            = sConfigMgr->GetOption<std::string>("HearthsideChat.Channel.Trade.MaxTier", "corpus");
    g_HsChannelTradeRatePerMin          = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Channel.Trade.RatePerMin", 3);
    g_HsChannelTradeMaxCandidates        = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Channel.Trade.MaxCandidates", 8);

    g_HsChannelGeneralMaxTier           = sConfigMgr->GetOption<std::string>("HearthsideChat.Channel.General.MaxTier", "corpus");
    g_HsChannelGeneralRatePerMin         = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Channel.General.RatePerMin", 3);
    g_HsChannelGeneralMaxCandidates       = sConfigMgr->GetOption<uint32_t>("HearthsideChat.Channel.General.MaxCandidates", 8);


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
    g_HsGeneratorLLMTemplate                 = sConfigMgr->GetOption<std::string>("HearthsideChat.Generator.LLM.Template", "llama3");
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
