#include "hs_botchain.h"
#include "hs_arbiter.h"
#include "hs_config.h" // every g_Hs* key below, and Hs_IsExcludedBotName
#include "hs_queue.h"  // HsReplyChannel's definition, Hs_TryEnqueue, the channel helpers
#include "hs_tier.h"
#include "hs_topic_gate.h"

#include "Channel.h" // Player::IsInChannel / Hs_ResolveChannelForDelivery's return
#include "DBCStores.h"
#include "Group.h"
#include "GroupReference.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h" // NewRpgStatus
#include "PlayerbotMgr.h"
#include "Random.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    // A scope untouched this long is forgotten: a disbanded group would
    // otherwise leave its entry in the map for the life of the process.
    // Pruning is opportunistic (see PruneStaleLocked) rather than on a timer;
    // this file has no WorldScript of its own, because it is driven by
    // delivery rather than by a scan.
    constexpr int64_t  kChainScopeStaleSeconds = 900;
    constexpr size_t   kChainPruneThreshold    = 64;

    struct ChainScope
    {
        uint32_t          depth        = 0;
        uint32_t          seq          = 0;
        Clock::time_point lastHopAt{};
        Clock::time_point lastTouchedAt{};
        bool              hasLastHop   = false;
    };

    std::mutex                                  g_ChainMutex;
    std::unordered_map<uint64_t, ChainScope>    g_Chains;
    std::atomic<uint32_t>                       g_HopsFiredThisSession{0};

    bool IsBot(Player* p)
    {
        if (!p)
            return false;
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(p);
        return ai && ai->IsBotAI();
    }

    // Caller holds g_ChainMutex. Only walks the map once it has grown past a
    // threshold, so the common case (a handful of live scopes) costs nothing.
    void PruneStaleLocked(Clock::time_point now)
    {
        if (g_Chains.size() < kChainPruneThreshold)
            return;
        for (auto it = g_Chains.begin(); it != g_Chains.end(); )
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastTouchedAt).count();
            if (elapsed >= kChainScopeStaleSeconds)
                it = g_Chains.erase(it);
            else
                ++it;
        }
    }

    // §4.13's remaining topic-gate facts. Read here on the world thread and
    // carried into the queued request as plain values, exactly as
    // hs_handler.cpp's BuildTopicGateContext and hs_engagement.cpp's
    // TryFireFollowUp already do: this file duplicates that read rather
    // than sharing it, per the convention hs_handler.cpp states at its own
    // copy (hs_topic_gate.h stays free of any AzerothCore dependency so it
    // can carry a standalone harness).
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

    // Party/raid candidates: the speaker's own group, subgroup-scoped for
    // party exactly as hs_handler.cpp's Group* hook scopes CHAT_MSG_PARTY
    // (the core broadcasts party chat by subgroup, so a member outside it
    // never heard the line and cannot be answering it). Also reports whether
    // a real player is in the group, collected on the same walk rather than
    // in a second pass.
    void CollectGroupCandidates(Player* speaker, Group* group, bool subgroupScoped,
                                 std::vector<Player*>& candidates, bool& sawRealPlayer)
    {
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || !member->IsInWorld() || member == speaker)
                continue;

            if (!IsBot(member))
            {
                sawRealPlayer = true;
                continue; // a real player is an audience, never a chain candidate
            }

            if (Hs_IsExcludedBotName(member->GetName()))
                continue;
            if (!member->IsAlive())
                continue;
            if (g_HsDisableRepliesInCombat && member->IsInCombat())
                continue;
            if (subgroupScoped && !group->SameSubGroup(speaker, member))
                continue;

            candidates.push_back(member);
        }
    }

    // Channel candidates. The scan is channel-membership-wide (a channel has
    // no proximity bound), so it is capped the same way hs_handler.cpp's
    // Channel* hook caps its own: zone-local first, then a bounded random
    // sample, via Hs_OrderChannelCandidates. Without that a hop would roll
    // the arbiter against every bot on the realm.
    void CollectChannelCandidates(Player* speaker, HsChannelKind kind,
                                   std::vector<Player*>& candidates, bool& sawRealPlayer)
    {
        Channel* channel = Hs_ResolveChannelForDelivery(speaker, kind);
        if (!channel)
            return;

        std::vector<Player*> eligible;
        for (auto const& itr : ObjectAccessor::GetPlayers())
        {
            Player* candidate = itr.second;
            if (!candidate || !candidate->IsInWorld() || candidate == speaker)
                continue;
            if (!candidate->IsInChannel(channel))
                continue;

            if (!IsBot(candidate))
            {
                sawRealPlayer = true;
                continue;
            }

            if (Hs_IsExcludedBotName(candidate->GetName()))
                continue;
            if (!candidate->IsAlive())
                continue;
            if (candidate->GetTeamId() != speaker->GetTeamId())
                continue; // opposing faction can't read this channel
            if (g_HsDisableRepliesInCombat && candidate->IsInCombat())
                continue;

            eligible.push_back(candidate);
        }
        if (eligible.empty())
            return;

        std::vector<HsChannelCandidate> ordered;
        ordered.reserve(eligible.size());
        for (Player* candidate : eligible)
            ordered.push_back({ candidate->GetGUID().GetRawValue(), candidate->GetZoneId() });

        uint64_t seed = (static_cast<uint64_t>(urand(0, 0xFFFFFFFFu)) << 32) | urand(0, 0xFFFFFFFFu);
        ordered = Hs_OrderChannelCandidates(ordered, speaker->GetZoneId(),
                                             Hs_ChannelPolicyFor(kind).maxCandidates, seed);

        for (HsChannelCandidate const& c : ordered)
            for (Player* candidate : eligible)
                if (candidate->GetGUID().GetRawValue() == c.guid)
                {
                    candidates.push_back(candidate);
                    break;
                }
    }
}

void Hs_NoteBotLine(Player* speaker, HsReplyChannel channel, HsChannelKind kind,
                     const std::string& text, bool wasChainHop)
{
    if (!g_HsEnable || !speaker || text.empty())
        return;

    // Live chaining is the one thing "inference" turns on that "corpus" does
    // not. Because a ceiling is permissive, "inference" still permits the
    // scripted replay path hs_script.cpp gates at HsTier::Corpus: the two
    // mechanisms run together, they do not replace each other.
    if (!HsTierAllows(HsParseTier(g_HsMaxTierBotToBot), HsTier::Inference))
        return;

    // Checked before the scope map is touched below, not just as part of the
    // depth gate: at MaxDepth 0 no hop can ever fire, and every party or
    // channel line would otherwise create a scope entry for the pruner to
    // clean up.
    if (g_HsBotChainMaxDepth == 0)
        return;

    // Resolve the surface to a scope, rejecting everything that does not
    // chain. Whisper is 1:1, guild is realm-wide with no bounded audience to
    // pace against, and /say is covered by the scripted mechanism: see the
    // header for why none of the three is a chaining surface.
    uint64_t scopeId       = 0;
    Group*   group         = nullptr;
    bool     subgroupScoped = false;

    if (channel == HsReplyChannel::Party || channel == HsReplyChannel::Raid)
    {
        group = speaker->GetGroup();
        if (!group)
            return; // the speaker left the group during its typing delay
        scopeId        = Hs_BotChainScopeForGroup(group->GetGUID().GetRawValue());
        subgroupScoped = (channel == HsReplyChannel::Party);
    }
    else if (channel == HsReplyChannel::Channel && kind == HsChannelKind::General)
    {
        // The channel's own ceiling still applies on top of MaxTier.BotToBot,
        // the same way TryFireChannelScript checks it independently.
        if (!HsTierAllows(Hs_ChannelPolicyFor(kind).maxTier, HsTier::Corpus))
            return;
        scopeId = Hs_BotChainScopeForChannel(kind);
    }
    else
    {
        return;
    }

    // ---- cheap gates first, all under one lock ----
    uint32_t depth = 0;
    uint32_t seq   = 0;
    {
        Clock::time_point now = Clock::now();
        std::lock_guard<std::mutex> lock(g_ChainMutex);
        PruneStaleLocked(now);

        ChainScope& scope   = g_Chains[scopeId];
        scope.lastTouchedAt = now;

        // A line that is not itself a hop was said to a real player, so it
        // starts a chain rather than continuing one.
        if (!wasChainHop)
            scope.depth = 0;

        if (scope.depth >= g_HsBotChainMaxDepth)
            return;

        // The cooldown paces whole chains, not the hops inside one: at depth
        // 0 a new chain has to wait out the scope's rest period, while a
        // chain already under way keeps its turns conversationally prompt.
        if (scope.depth == 0 && scope.hasLastHop)
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - scope.lastHopAt).count();
            if (elapsed < static_cast<int64_t>(g_HsBotChainScopeCooldownSeconds))
                return;
        }

        depth = scope.depth;
        seq   = scope.seq;
    }

    if (urand(0, 99) >= Hs_BotChainHopChancePercent(g_HsBotChainBaseChancePercent,
                                                     g_HsBotChainDecayPercent, depth))
        return;

    // A hop is a line in the channel like any other, so it spends a token
    // from that channel's bucket. Taken after the chance roll (a roll that
    // misses costs nothing) and before the realm-wide scan below.
    if (channel == HsReplyChannel::Channel && !Hs_ChannelBucketTake(kind))
        return;

    bool                 sawRealPlayer = false;
    std::vector<Player*> candidates;
    if (group)
        CollectGroupCandidates(speaker, group, subgroupScoped, candidates, sawRealPlayer);
    else
        CollectChannelCandidates(speaker, kind, candidates, sawRealPlayer);

    // Bots talking to each other with nobody there to overhear it is pure GPU
    // spend against no one's experience: the same reasoning that scopes
    // guild replies to a guild with a real member online.
    if (g_HsBotChainRequireRealPlayer && !sawRealPlayer)
        return;
    if (candidates.empty())
        return;

    std::vector<Player*> selected = Hs_ArbitrateReplies(speaker, text, candidates);
    if (selected.empty())
        return;

    // Exactly one, even though the arbiter may offer two: a second bot
    // answering the same line would fork the chain into two branches sharing
    // one depth counter, and the depth cap would stop meaning what it says.
    Player* responder = selected[0];

    uint64_t responderGuid = responder->GetGUID().GetRawValue();
    bool     inCombat      = responder->IsInCombat();
    uint8_t  botLevel      = responder->GetLevel();

    NewRpgStatus rpgStatus = RPG_IDLE;
    if (PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(responder))
        rpgStatus = botAI->rpgInfo.GetStatus();

    // isEvent, not isFollowUp: hs_queue.h documents that flag as the one for
    // a request whose "sender" is another bot, and that is exactly this case.
    // It suppresses the history write, the interaction-score bump, the
    // engagement re-arm, the distracted-reply roll, and: the part only this
    // flag carries: Hs_EnsureFirstMeetingRecorded, so a chain can never
    // seed identity state from two bots meeting each other.
    bool admitted = Hs_TryEnqueue(responderGuid, responder->GetName(),
                                   speaker->GetGUID().GetRawValue(), speaker->GetName(),
                                   channel, text, inCombat, botLevel, rpgStatus,
                                   BuildTopicGateContext(responder),
                                   /*isFollowUp=*/false, /*isEvent=*/true,
                                   kind, scopeId, seq);
    if (!admitted)
        return; // bucket/cooldown/breaker/queue-depth: silence, not a retry

    {
        Clock::time_point now = Clock::now();
        std::lock_guard<std::mutex> lock(g_ChainMutex);
        auto it = g_Chains.find(scopeId);
        if (it == g_Chains.end())
            return; // pruned between the gate above and here: nothing to record
        // Re-check the generation: a player may have taken the floor while
        // the scan above was running. The hop still carries the older seq, so
        // delivery will drop it; not advancing depth here keeps the scope's
        // bookkeeping consistent with that.
        if (it->second.seq != seq)
            return;
        it->second.depth        += 1;
        it->second.lastHopAt     = now;
        it->second.lastTouchedAt = now;
        it->second.hasLastHop    = true;
    }

    g_HopsFiredThisSession.fetch_add(1);
}

void Hs_AbortBotChainsInScope(uint64_t scopeId)
{
    std::lock_guard<std::mutex> lock(g_ChainMutex);
    auto it = g_Chains.find(scopeId);
    if (it == g_Chains.end())
        return; // no chain here: nothing to take the floor from

    // Bumping seq is what actually drops the hop still generating: it carries
    // the old value, and Hs_BotChainHopStillValid rejects it at delivery.
    // lastHopAt is deliberately left alone: a chain that just ran still owes
    // the scope its cooldown before the player's arrival can seed a new one.
    it->second.seq          += 1;
    it->second.depth         = 0;
    it->second.lastTouchedAt = Clock::now();
}

bool Hs_BotChainHopStillValid(uint64_t scopeId, uint32_t chainSeq)
{
    std::lock_guard<std::mutex> lock(g_ChainMutex);
    auto it = g_Chains.find(scopeId);
    if (it == g_Chains.end())
        return false; // scope forgotten entirely: there is no chain left to continue
    return it->second.seq == chainSeq;
}

uint32_t Hs_BotChainHopsFiredThisSession()
{
    return g_HopsFiredThisSession.load();
}
