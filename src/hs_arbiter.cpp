#include "hs_arbiter.h"
#include "hs_config.h"
#include "hs_queue.h"

#include "Player.h"
#include "Random.h"

#include <algorithm>
#include <cctype>
#include <cstdint>


namespace
{
    // Whole-word, case-insensitive search for botName inside msg.
    bool MentionsName(const std::string& msg, const std::string& botName)
    {
        auto lower = [](std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
        std::string lowerMsg  = lower(msg);
        std::string lowerName = lower(botName);
        if (lowerName.empty())
            return false;

        size_t pos = 0;
        while ((pos = lowerMsg.find(lowerName, pos)) != std::string::npos)
        {
            bool startOk = (pos == 0) || !std::isalnum(static_cast<unsigned char>(lowerMsg[pos - 1]));
            size_t endPos = pos + lowerName.size();
            bool endOk = (endPos >= lowerMsg.size()) || !std::isalnum(static_cast<unsigned char>(lowerMsg[endPos]));
            if (startOk && endOk)
                return true;
            ++pos;
        }
        return false;
    }

    // Weighted toward 0 and 1, 2 rare by default: tunable via the
    // HearthsideChat.ReplyCount.{Zero,One,Two}Percent weights (hs_config.h).
    // These are relative weights, not cumulative percentages: summed and
    // normalized here, so they need not add to 100 and carry no ordering
    // constraint between them.
    uint32_t PickReplyCount(size_t eligibleCount)
    {
        if (eligibleCount == 0)
            return 0;

        uint32_t weights[3] = { g_HsReplyCountZeroPercent, g_HsReplyCountOnePercent, g_HsReplyCountTwoPercent };
        uint32_t total      = weights[0] + weights[1] + weights[2];
        if (total == 0)
            return 0;

        uint32_t roll       = urand(0, total - 1);
        uint32_t cumulative = 0;
        uint32_t count      = 2;
        for (uint32_t i = 0; i < 3; ++i)
        {
            cumulative += weights[i];
            if (roll < cumulative)
            {
                count = i;
                break;
            }
        }
        return static_cast<uint32_t>(std::min<size_t>(count, eligibleCount));
    }

    // Recent-speaker penalty: a bot that just answered is down-weighted
    // for a short window so the same bot doesn't answer three lines
    // running. No archetype/ring multipliers yet: proximity + recency
    // is the whole weighting model until they land.
    double RecencyWeight(uint32_t secondsSinceLastReply)
    {
        constexpr uint32_t window    = 60;
        constexpr double   minFactor = 0.15;
        if (secondsSinceLastReply >= window)
            return 1.0;

        double t = static_cast<double>(secondsSinceLastReply) / static_cast<double>(window);
        return minFactor + (1.0 - minFactor) * t;
    }

    // Falls back to the beyond-range floor for a cross-map candidate --
    // party/raid/guild chat can span maps, and GetDistance() across
    // unrelated coordinate spaces is meaningless, not just "far" (§4.15).
    double ProximityWeight(Player* speaker, Player* candidate)
    {
        if (candidate->GetMapId() != speaker->GetMapId())
            return 1.0;

        float distance = candidate->GetDistance(speaker);
        float falloff  = g_HsSayDistance - distance;
        return static_cast<double>(std::max(1.0f, falloff));
    }
}

std::vector<Player*> Hs_ArbitrateReplies(Player* speaker, const std::string& message, const std::vector<Player*>& candidates)
{
    std::vector<Player*> selected;
    if (!speaker || candidates.empty())
        return selected;

    // Named address wins outright: no lottery, no substitutes.
    for (Player* bot : candidates)
    {
        if (MentionsName(message, bot->GetName()))
        {
            selected.push_back(bot);
            return selected;
        }
    }

    // Pick a reply count, which may be zero. No per-candidate willingness
    // filter runs ahead of this any more: hside_archetype.reply_chance was
    // replaced outright by distracted_chance on 2026-08-24 (hs_archetype.h).
    // A personality that often just doesn't answer is true to a real player
    // but reads as being ignored: or as a broken module: on a realm that
    // is almost entirely bots, so the trait is now expressed as a *late*
    // reply (hs_queue.cpp's "sorry, was afk" line) rather than a missing one.
    // PickReplyCount's own weighting is still what keeps a crowd from
    // answering in chorus.
    uint32_t replyCount = PickReplyCount(candidates.size());
    if (replyCount == 0)
        return selected;

    // Weighted select without replacement: proximity and recency, not
    // uniform.
    //
    // Review G2: this used to construct a std::random_device plus a
    // std::mt19937 here, per call -- and this function runs once per /say,
    // party, raid, guild and channel message on the realm. random_device is
    // a syscall (or a /dev/urandom open) and seeding mt19937 initializes
    // 2.5KB of state, all to draw at most two numbers.
    // hs_event_arbiter.cpp already solved exactly this and documents why;
    // the /say arbiter simply never got the same treatment.
    //
    // urand() rather than a file-static engine: AzerothCore's urand is
    // backed by a stateless RandomEngine over a thread_local SFMTRand
    // (Random.cpp), so it needs no seeding, no lock, and is safe from the
    // world thread and the queue worker alike. The distribution below is
    // continuous, so the roll is built from a 32-bit urand scaled into
    // [0, total) rather than from uniform_real_distribution.
    std::vector<Player*> pool = candidates;

    for (uint32_t i = 0; i < replyCount && !pool.empty(); ++i)
    {
        std::vector<double> weights;
        weights.reserve(pool.size());
        double total = 0.0;
        for (Player* bot : pool)
        {
            double w = ProximityWeight(speaker, bot) * RecencyWeight(Hs_SecondsSinceLastReply(bot->GetGUID().GetRawValue()));
            weights.push_back(w);
            total += w;
        }

        // urand(0, 0xFFFFFFFFu) is the full 32-bit range (verified safe:
        // AzerothCore's urand asserts only max >= min, unlike TrinityCore's
        // ASSERT(INT_MAX >= max)). Dividing by 2^32 gives a uniform double
        // in [0, 1), scaled to [0, total) -- the same half-open interval
        // uniform_real_distribution(0.0, total) produced.
        double unit       = static_cast<double>(urand(0, 0xFFFFFFFFu)) / 4294967296.0;
        double roll       = unit * total;
        size_t pickIndex  = pool.size() - 1;
        double cumulative = 0.0;
        for (size_t idx = 0; idx < weights.size(); ++idx)
        {
            cumulative += weights[idx];
            if (roll < cumulative)
            {
                pickIndex = idx;
                break;
            }
        }

        selected.push_back(pool[pickIndex]);
        pool.erase(pool.begin() + static_cast<long>(pickIndex));
    }

    return selected;
}
