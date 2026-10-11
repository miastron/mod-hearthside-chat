#include "hs_arbiter.h"
#include "hs_config.h"
#include "hs_queue.h"
#include "hs_weight.h"

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

    // Falls back to the beyond-range floor for a cross-map candidate --
    // party/raid/guild chat can span maps, and GetDistance() across
    // unrelated coordinate spaces is meaningless, not just "far" (§4.15).
    double ProximityWeight(Player* speaker, Player* candidate)
    {
        if (candidate->GetMapId() != speaker->GetMapId())
            return 1.0;
        return Hs_ProximityWeight(g_HsSayDistance, candidate->GetDistance(speaker));
    }
}

bool Hs_AddressesSomeoneElse(const std::string& message, const std::vector<Player*>& bots,
                             const std::vector<Player*>& humans)
{
    for (Player* bot : bots)
        if (MentionsName(message, bot->GetName()))
            return false;
    for (Player* human : humans)
        if (MentionsName(message, human->GetName()))
            return true;
    return false;
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
    // uniform. urand() is thread_local and needs no seeding, so it is safe
    // from the world thread and the queue worker alike.
    std::vector<Player*> pool = candidates;

    for (uint32_t i = 0; i < replyCount && !pool.empty(); ++i)
    {
        std::vector<double> weights;
        weights.reserve(pool.size());
        double total = 0.0;
        for (Player* bot : pool)
        {
            double w = ProximityWeight(speaker, bot) * Hs_RecencyWeight(Hs_SecondsSinceLastReply(bot->GetGUID().GetRawValue()));
            weights.push_back(w);
            total += w;
        }

        // The full 32-bit urand over 2^32: a uniform double in [0, 1).
        double unit      = static_cast<double>(urand(0, 0xFFFFFFFFu)) / 4294967296.0;
        size_t pickIndex = Hs_WeightedIndex(weights, unit * total);

        selected.push_back(pool[pickIndex]);
        pool.erase(pool.begin() + static_cast<long>(pickIndex));
    }

    return selected;
}
