#include "hs_arbiter.h"
#include "hs_config.h"
#include "hs_queue.h"

#include "Player.h"
#include "Random.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <random>

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

    // Weighted toward 0 and 1, 2 rare. Compiled constants, not config --
    // this tunes the illusion, not a GPU dial.
    uint32_t PickReplyCount(size_t eligibleCount)
    {
        if (eligibleCount == 0)
            return 0;

        uint32_t roll  = urand(0, 99);
        uint32_t count = (roll < 50) ? 0 : (roll < 92) ? 1 : 2;
        return static_cast<uint32_t>(std::min<size_t>(count, eligibleCount));
    }

    // Recent-speaker penalty: a bot that just answered is down-weighted
    // for a short window so the same bot doesn't answer three lines
    // running. No archetype/ring multipliers yet -- proximity + recency
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

    double ProximityWeight(Player* speaker, Player* candidate)
    {
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

    // Named address wins outright -- no lottery, no substitutes.
    for (Player* bot : candidates)
    {
        if (MentionsName(message, bot->GetName()))
        {
            selected.push_back(bot);
            return selected;
        }
    }

    // Pick a reply count, which may be zero.
    uint32_t replyCount = PickReplyCount(candidates.size());
    if (replyCount == 0)
        return selected;

    // Weighted select without replacement -- proximity and recency, not
    // uniform.
    std::vector<Player*> pool = candidates;
    std::random_device   rd;
    std::mt19937          gen(rd());

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

        std::uniform_real_distribution<double> dist(0.0, total);
        double roll       = dist(gen);
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
