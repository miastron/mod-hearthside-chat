#include "hs_ambient.h"
#include "hs_archetype.h"
#include "hs_channel.h"
#include "hs_config.h"
#include "hs_corpus.h"
#include "hs_identity_store.h"
#include "hs_prune.h"
#include "hs_queue.h"
#include "hs_rpgstate.h"
#include "hs_script.h"
#include "hs_style.h"
#include "hs_tier.h"

#include "Channel.h"
#include "Group.h"
#include "GroupReference.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Random.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    // Matches hs_script.cpp:38 and hs_engagement.cpp's own scan cadence.
    // Deliberately the same number rather than a new one: three periodic
    // scans on three different periods would make their interaction
    // impossible to reason about, and there is no reason ambient needs to
    // look more often than the scripted-scene producer it shares a budget
    // with.
    constexpr uint32_t kScanIntervalMs = 30000;

    // Upper bound on how many candidates a single scan collects before it
    // stops walking. The scan picks uniformly among what it collected, so
    // stopping early biases toward whatever ObjectAccessor happens to
    // enumerate first -- accepted for the same reason hs_script.cpp accepts
    // "first eligible instance found this scan": the enumeration order is
    // effectively arbitrary, this runs every 30 seconds, and the alternative
    // is an unbounded O(bots x players) walk on a realm with thousands of
    // bots. The cap only binds on realms large enough for the walk to matter.
    constexpr size_t kMaxCandidates = 32;

    // Which surface a given tick speaks on. Not HsReplyChannel: that enum is
    // about *delivery*, and it collapses the three global channels into a
    // single Channel value carrying an HsChannelKind. Ambient needs to choose
    // among five things that each have their own scan, so it needs its own
    // five-valued list.
    enum class AmbientSurface
    {
        Say,
        Party,
        Raid,
        Trade,
        General,
    };

    std::atomic<uint32_t> g_AmbientLinesFired{0};

    uint32_t g_ScanAccumulatorMs = 0;

    // ---- per-bot ambient cooldown ----
    // Keyed by bot GUID alone, unlike hs_opener.cpp's (bot, player) pair map.
    // An opener is addressed to someone, so "has this bot greeted *this
    // player* recently" is the right question; an ambient line is addressed
    // to nobody, so the only question is how recently this bot last spoke
    // into the void. That also keeps the map bounded by the bot population
    // rather than its product with the player population.
    std::mutex                                      g_CooldownMutex;
    std::unordered_map<uint64_t, Clock::time_point>  g_LastAmbientAt;

    bool IsBot(Player* p)
    {
        if (!p)
            return false;
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(p);
        return ai && ai->IsBotAI();
    }

    // HearthsideChat.ExcludeNames -- "no reflex, grounded, corpus, or
    // reactive reply, ever" (hs_config.h). An ambient line is corpus content
    // spoken unprompted, so an excluded bot may never be the speaker. Same
    // predicate hs_script.cpp uses, and for the same reason: unlike
    // hs_opener.cpp, no scan in this file needs to tell an excluded bot from
    // a real player, so one predicate suffices.
    bool IsEligibleBot(Player* p)
    {
        return IsBot(p) && !Hs_IsExcludedBotName(p->GetName());
    }

    bool AmbientCooldownOk(uint64_t botGuid)
    {
        std::lock_guard<std::mutex> lock(g_CooldownMutex);
        auto it = g_LastAmbientAt.find(botGuid);
        if (it == g_LastAmbientAt.end())
            return true;
        auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - it->second).count();
        return elapsedSec >= static_cast<int64_t>(g_HsAmbientBotCooldownSeconds);
    }

    void MarkAmbientFired(uint64_t botGuid)
    {
        std::lock_guard<std::mutex> lock(g_CooldownMutex);
        Clock::time_point now = Clock::now();
        g_LastAmbientAt[botGuid] = now;

        // Read only through g_HsAmbientBotCooldownSeconds, so an entry four
        // windows old already answers exactly as a missing one does
        // (hs_prune.h). Bounded by the bot population rather than a product,
        // so this is retention hygiene on a long-uptime realm rather than a
        // correctness need -- hence the same generous multiplier the other
        // call sites use.
        HsPrune::PruneStale(g_LastAmbientAt, now,
                            /*staleSeconds=*/static_cast<int64_t>(g_HsAmbientBotCooldownSeconds) * 4,
                            /*pruneAboveSize=*/512);
    }

    // Every gate that depends only on the bot itself, in ascending cost
    // order. The surface-specific scans below add their own audience test on
    // top of this.
    bool BotBaseEligible(Player* bot)
    {
        if (!bot || !bot->IsInWorld() || !IsEligibleBot(bot))
            return false;
        if (!bot->IsAlive() || bot->IsInCombat())
            return false;
        // A bot mid-scene must not interrupt itself to muse aloud. The two
        // producers share speakers and a 30-second cadence, so without this
        // they would collide regularly rather than rarely.
        if (Hs_IsBotInAnyScriptRun(bot->GetGUID().GetRawValue()))
            return false;
        return AmbientCooldownOk(bot->GetGUID().GetRawValue());
    }

    // The one funnel every surface converges on: placeholder resolution,
    // style pass, delivery, bookkeeping. Mirrors hs_handler.cpp's
    // TryCorpusFallback exactly -- ambient is the same zero-GPU corpus
    // delivery, differing only in that nothing asked for it.
    //
    // Scores nothing and writes no history, deliberately, for the same
    // reason hs_opener.cpp states for openers and hs_queue.cpp states for
    // channel replies: a bot talking to itself is not a relationship, and
    // letting it feed interaction_score would promote bots toward carded
    // identity purely for being noisy.
    void SpeakAmbient(Player* bot, std::string line, HsReplyChannel channel,
                       HsChannelKind channelKind, const std::string& listenerName)
    {
        uint64_t botGuid = bot->GetGUID().GetRawValue();

        HsCardSnapshot snapshot = Hs_LookupCardSnapshot(botGuid);

        // Card-only placeholders (%main_focus, %current_goal). Only the /say
        // pool can contain them -- the party/raid and channel category
        // queries both filter card_gated = 0 -- but running the pass
        // unconditionally is a plain substring replace over a line that
        // almost never contains either token, and it means a future
        // card_gated category on those surfaces can't silently deliver a
        // half-resolved line.
        if (snapshot.active)
            line = Hs_ResolveCardPlaceholders(line, snapshot.mainFocus, snapshot.currentGoal);

        // Universal placeholders, after the card pass so the leftover check
        // sees a fully-substituted line. An unresolvable token drops the
        // line into silence rather than an untrue claim (§4.13) -- and note
        // the ambient token is already spent at this point, which is
        // correct: the budget bounds how often a bot *attempts* to speak,
        // and a dropped line is silence the player experiences, not free.
        if (line.find('%') != std::string::npos)
        {
            if (!Hs_ResolveUniversalPlaceholders(line, Hs_BuildPlaceholderContext(bot)))
                return;
        }

        HsArchetype           archetype     = Hs_ArchetypeForBot(botGuid, bot->GetLevel());
        HsArchetypeInfo const archetypeInfo = Hs_ArchetypeInfoFor(archetype);
        HsStyleContext styleCtx;
        styleCtx.baselineCare         = archetypeInfo.care;
        styleCtx.abbrevOverrideChance = archetypeInfo.hasAbbrevOverride ? archetypeInfo.abbrevOverrideChance : -1.0f;
        styleCtx.inCombat             = false; // BotBaseEligible already excluded in-combat bots
        styleCtx.verbalTic            = snapshot.verbalTic;
        styleCtx.tradeCareOffset      = Hs_TradeCareOffsetFor(botGuid);

        HsStyleResult style = Hs_ApplyStyle(botGuid, bot->GetName(), listenerName, line, styleCtx);
        if (style.text.empty())
            return;

        // senderGuid 0: ambient has no addressee. Hs_DeliverReflexReply only
        // reads it on the Whisper path (to resolve the recipient), and
        // ambient never whispers -- an unprompted whisper to a stranger is
        // the one shape of this feature that would read as harassment rather
        // than atmosphere.
        Hs_DeliverReflexReply(botGuid, /*senderGuid=*/0, channel, style.text, channelKind);

        MarkAmbientFired(botGuid);
        g_AmbientLinesFired.fetch_add(1);
    }

    // ---- /say ----------------------------------------------------------
    // Dead air near a real player. The one surface where "is anyone actually
    // there" has to be tested explicitly and by distance, which is why
    // Ambient.RequireRealPlayer exists at all -- party/raid get the same
    // guarantee free from the delivery layer, and the channel scan tests
    // membership instead.
    //
    // Four independent conditions now have to line up, and the order below
    // is deliberate (cheapest first, each cutting the pool the next one
    // walks):
    //
    //   1. the bot is settled -- stationary, and in RPG_REST or
    //      RPG_WANDER_NPC (Hs_IsBotSettled, hs_rpgstate.h). A bot sprinting
    //      to a quest objective muttering about the scenery is the tell
    //      this gate exists to remove.
    //   2. a real player is in earshot (Say.Distance), as before.
    //   3. another eligible bot is in earshot too. Musing aloud with
    //      literally nobody but the player around reads as the bot talking
    //      *at* them and expecting an answer; with a companion present the
    //      same line reads as overheard, which is what it is meant to be.
    //      The companion is not required to be settled -- it is scenery
    //      here, not a participant, and requiring both would cut the pool
    //      by the square of an already narrow fraction.
    //   4. the roll (Ambient.Say.FireChancePercent).
    //
    // The chance is high precisely because the first three are narrow --
    // see hs_config.h's block on the three fire-chance keys.
    void TryAmbientSay()
    {
        std::vector<Player*> realPlayers;
        std::vector<Player*> bots;
        for (auto const& itr : ObjectAccessor::GetPlayers())
        {
            Player* candidate = itr.second;
            if (!candidate || !candidate->IsInWorld())
                continue;
            // Three-way, not two: an excluded bot is neither a valid speaker
            // nor a real player. Letting it fall into realPlayers would make
            // other bots treat it as the human whose presence justifies the
            // line -- the same trap hs_opener.cpp documents for its own scan.
            if (IsBot(candidate))
            {
                if (IsEligibleBot(candidate))
                    bots.push_back(candidate);
            }
            else
                realPlayers.push_back(candidate);
        }

        // Two, not one: gate 3 above needs a speaker *and* a companion, so a
        // realm with a single eligible bot in the world can never satisfy it
        // and there is nothing to walk.
        if (bots.size() < 2)
            return;
        if (g_HsAmbientRequireRealPlayer && realPlayers.empty())
            return;

        // (speaker, the nearby player whose name the style pass protects).
        std::vector<std::pair<Player*, Player*>> candidates;
        for (Player* bot : bots)
        {
            if (!BotBaseEligible(bot))
                continue;

            // Gate 1. A cheap map lookup, so it goes ahead of both distance
            // walks below and cuts the pool they have to cover.
            if (!Hs_IsBotSettled(bot))
                continue;

            Player* audience = nullptr;
            for (Player* player : realPlayers)
            {
                if (bot->GetTeamId() != player->GetTeamId())
                    continue;
                // Map- and phase-aware; see hs_handler.cpp's /say
                // eligibility filter for why a bare GetDistance is wrong.
                if (!bot->IsWithinDistInMap(player, g_HsSayDistance))
                    continue;
                audience = player;
                break; // one is enough -- this is a presence test, not a count
            }

            if (!audience && g_HsAmbientRequireRealPlayer)
                continue;

            // Gate 3. Same presence-not-count shape as the audience scan
            // above, and the same map/phase-aware distance test -- one
            // companion in earshot is the whole requirement.
            bool hasCompanion = false;
            for (Player* other : bots)
            {
                if (other == bot)
                    continue;
                if (other->GetTeamId() != bot->GetTeamId())
                    continue;
                if (!other->IsAlive()) // a corpse is not company
                    continue;
                if (!bot->IsWithinDistInMap(other, g_HsSayDistance))
                    continue;
                hasCompanion = true;
                break;
            }
            if (!hasCompanion)
                continue;

            candidates.emplace_back(bot, audience);
            if (candidates.size() >= kMaxCandidates)
                break;
        }

        if (candidates.empty())
            return;

        auto const& picked = candidates[urand(0, static_cast<uint32_t>(candidates.size() - 1))];
        Player* speaker  = picked.first;
        Player* audience = picked.second;

        // Gate 4, ahead of the budget for the same reason hs_opener.cpp
        // rolls before Hs_AmbientBucketTake: a token should be spent on a
        // line actually about to be attempted, not burned by a speaker the
        // roll was going to silence anyway.
        if (urand(0, 99) >= g_HsAmbientSayFireChancePercent)
            return;

        // Budget spent only now, on a speaker that is actually going to try.
        // PLAN-AMBIENT.md §5 sketched this check ahead of speaker selection;
        // taken there it would burn a token on every tick that found nobody
        // eligible, which on a quiet realm is most of them.
        if (!Hs_AmbientBucketTake())
            return;

        HsCardSnapshot snapshot = Hs_LookupCardSnapshot(speaker->GetGUID().GetRawValue());
        std::string line = Hs_SelectCorpusLine(speaker->getClass(), speaker->GetLevel(),
                                                static_cast<uint8_t>(speaker->GetTeamId()),
                                                speaker->GetZoneId(), snapshot.active);
        if (line.empty())
            return;

        SpeakAmbient(speaker, line, HsReplyChannel::Say, HsChannelKind::Trade,
                      audience ? audience->GetName() : "");
    }

    // ---- party / raid --------------------------------------------------
    // PlayerbotAI::SayToParty and ::SayToRaid send only to
    // GetRealPlayersInGroup(), so a bot-only group generates no packets at
    // all -- the real-player gate is enforced by the delivery layer here
    // regardless of Ambient.RequireRealPlayer. The scan still checks for a
    // human in the group, unconditionally and not gated on that key: a line
    // nobody receives would still spend a token from a realm-wide budget and
    // still burn the speaker's half-hour cooldown, so letting it through
    // would quietly starve the surfaces that do reach someone.
    void TryAmbientGroup(bool isRaid)
    {
        std::vector<Player*> candidates;
        for (auto const& itr : ObjectAccessor::GetPlayers())
        {
            Player* bot = itr.second;
            if (!BotBaseEligible(bot))
                continue;

            Group* group = bot->GetGroup();
            if (!group)
                continue;
            if (group->isRaidGroup() != isRaid)
                continue;

            bool hasRealPlayer = false;
            for (GroupReference* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == bot || !member->IsInWorld())
                    continue;
                if (!IsBot(member))
                {
                    hasRealPlayer = true;
                    break;
                }
            }
            if (!hasRealPlayer)
                continue;

            candidates.push_back(bot);
            if (candidates.size() >= kMaxCandidates)
                break;
        }

        if (candidates.empty())
            return;

        Player* speaker = candidates[urand(0, static_cast<uint32_t>(candidates.size() - 1))];

        if (!Hs_AmbientBucketTake())
            return;

        std::string line = Hs_SelectGroupAmbientLine(isRaid, speaker->getClass(), speaker->GetLevel(),
                                                       static_cast<uint8_t>(speaker->GetTeamId()),
                                                       speaker->GetZoneId());
        if (line.empty())
            return;

        // No listener name to protect: a party line is addressed to the
        // group, not to one person, so there is no name the style pass must
        // keep intact. Same reasoning hs_script.cpp's channel delivery uses
        // for passing an empty sender.
        SpeakAmbient(speaker, line, isRaid ? HsReplyChannel::Raid : HsReplyChannel::Party,
                      HsChannelKind::Trade, "");
    }

    // ---- Trade / General -----------------------------------------------
    // Membership is the audience test here rather than distance. Bots are
    // grouped by resolved Channel* -- the same zone-qualified resolution
    // delivery itself uses, so "same resolved Channel*" is equivalent to
    // "members of the same channel instance" (hs_script.cpp's channel scan
    // documents this equivalence and it holds identically here).
    void TryAmbientChannel(HsChannelKind kind)
    {
        // The channel's own MaxTier, independent of MaxTier.Ambient: an
        // operator who set Channel.General.MaxTier = off meant it, and ambient
        // is not an exception to that.
        if (!HsTierAllows(Hs_ChannelPolicyFor(kind).maxTier, HsTier::Corpus))
            return;

        std::unordered_map<Channel*, std::vector<Player*>> botsByInstance;
        std::vector<Player*>                                realPlayers;

        for (auto const& itr : ObjectAccessor::GetPlayers())
        {
            Player* candidate = itr.second;
            if (!candidate || !candidate->IsInWorld())
                continue;

            // Real players are collected unresolved, deliberately. It is
            // tempting to resolve their channel here too and group everyone
            // in one pass, but that would make this scan's correctness depend
            // on Hs_ResolveChannelForDelivery agreeing with the core about
            // every channel's name -- and a disagreement there resolves a
            // human to the wrong instance or to nullptr, which silences the
            // surface outright rather than failing visibly. (That is not
            // hypothetical: the city-scoped names disagreed until the
            // AreaID 3459 fix, and Trade carried no traffic at all for it.)
            // Membership is tested below against the bot-resolved Channel*
            // instead, which is exact because it is the same object, whatever
            // that object happens to be called.
            if (!IsBot(candidate))
            {
                realPlayers.push_back(candidate);
                continue;
            }

            // Every cheap gate precedes the resolve:
            // Hs_ResolveChannelForDelivery is a DBC lookup plus a ChannelMgr
            // string match, by far the most expensive test in this loop.
            if (!BotBaseEligible(candidate))
                continue;

            Channel* channel = Hs_ResolveChannelForDelivery(candidate, kind);
            if (!channel || !candidate->IsInChannel(channel))
                continue;

            std::vector<Player*>& pool = botsByInstance[channel];
            if (pool.size() < kMaxCandidates)
                pool.push_back(candidate);
        }

        if (botsByInstance.empty())
            return;
        if (g_HsAmbientRequireRealPlayer && realPlayers.empty())
            return;

        // Collect the instances that can actually carry a line, then pick
        // one -- rather than taking the first eligible instance found, which
        // on a realm with several populated cities would let whichever
        // instance enumerated first monopolize the surface.
        std::vector<std::vector<Player*>*> eligibleInstances;
        for (auto& entry : botsByInstance)
        {
            if (entry.second.empty())
                continue;

            if (g_HsAmbientRequireRealPlayer)
            {
                bool heard = false;
                for (Player* player : realPlayers)
                {
                    if (player->IsInChannel(entry.first))
                    {
                        heard = true;
                        break; // presence test, not a count
                    }
                }
                if (!heard)
                    continue;
            }

            eligibleInstances.push_back(&entry.second);
        }
        if (eligibleInstances.empty())
            return;

        std::vector<Player*>* pool =
            eligibleInstances[urand(0, static_cast<uint32_t>(eligibleInstances.size() - 1))];
        Player* speaker = (*pool)[urand(0, static_cast<uint32_t>(pool->size() - 1))];

        // This channel's own rate limit before the shared budget: it is the
        // narrower constraint, and a channel throttled by its own RatePerMin
        // should not also be spending from the realm-wide ambient budget
        // other surfaces are waiting on.
        if (!Hs_ChannelBucketTake(kind))
            return;
        if (!Hs_AmbientBucketTake())
            return;

        std::string line = Hs_SelectChannelLine(kind, speaker->getClass(), speaker->GetLevel(),
                                                 static_cast<uint8_t>(speaker->GetTeamId()),
                                                 speaker->GetZoneId());
        if (line.empty())
            return;

        SpeakAmbient(speaker, line, HsReplyChannel::Channel, kind, "");
    }

    void ScanAmbient()
    {
        // Corpus-only, permanently -- see hs_ambient.h. "inference" is
        // accepted and behaves exactly as "corpus"; there is no generated
        // ambient path and no plan for one.
        if (!HsTierAllows(HsParseTier(g_HsMaxTierAmbient), HsTier::Corpus))
            return;

        // Which surfaces are switched on at all. Cheap config and policy
        // reads only -- no player walk happens until one surface is chosen,
        // which is what keeps a tick to at most one channel resolution pass.
        std::vector<AmbientSurface> enabled;
        if (g_HsAmbientSayEnable)
            enabled.push_back(AmbientSurface::Say);
        if (g_HsAmbientPartyEnable)
            enabled.push_back(AmbientSurface::Party);
        if (g_HsAmbientRaidEnable)
            enabled.push_back(AmbientSurface::Raid);
        if (HsTierAllows(Hs_ChannelPolicyFor(HsChannelKind::Trade).maxTier, HsTier::Corpus))
            enabled.push_back(AmbientSurface::Trade);
        if (HsTierAllows(Hs_ChannelPolicyFor(HsChannelKind::General).maxTier, HsTier::Corpus))
            enabled.push_back(AmbientSurface::General);

        if (enabled.empty())
            return;

        // One surface per tick, chosen uniformly among those enabled rather
        // than scanning them all. Two reasons, and the second is the load-
        // bearing one:
        //
        //   - Cost. Only the chosen surface's scan runs, so a tick pays for
        //     at most one channel resolution pass.
        //   - Fairness. Scanning every surface and letting the shared budget
        //     sort it out would mean whichever surface happened to be checked
        //     first always won, and the later ones would only ever speak on
        //     the ticks the earlier ones found nobody. Choosing first makes
        //     each enabled surface equally likely to be the one that speaks,
        //     independent of how many bots each has available -- so /say,
        //     with the largest pool by far, cannot drown out party chatter.
        switch (enabled[urand(0, static_cast<uint32_t>(enabled.size() - 1))])
        {
            case AmbientSurface::Say:     TryAmbientSay();                          break;
            case AmbientSurface::Party:   TryAmbientGroup(/*isRaid=*/false);          break;
            case AmbientSurface::Raid:    TryAmbientGroup(/*isRaid=*/true);           break;
            case AmbientSurface::Trade:   TryAmbientChannel(HsChannelKind::Trade);    break;
            case AmbientSurface::General: TryAmbientChannel(HsChannelKind::General);  break;
        }
    }
}

void HsAmbientScanWorldScript::OnUpdate(uint32_t diff)
{
    if (!g_HsEnable)
        return;

    g_ScanAccumulatorMs += diff;
    if (g_ScanAccumulatorMs < kScanIntervalMs)
        return;
    g_ScanAccumulatorMs = 0;

    ScanAmbient();
}

uint32_t Hs_AmbientLinesFiredThisSession()
{
    return g_AmbientLinesFired.load();
}

void Hs_MarkAmbientSpoke(uint64_t botGuid)
{
    MarkAmbientFired(botGuid);
}
