#include "hs_topic_gate.h"

namespace
{
    // Player::GetMoney() is copper; 1g = 100s = 10000c. Silver-only precision
    // matches how a player would actually describe their own purse: nobody
    // states copper in chat.
    constexpr uint32_t kCopperPerSilver = 100;
    constexpr uint32_t kCopperPerGold   = 10000;
}

std::string Hs_TopicGateLine(const HsTopicGateContext& ctx)
{
    std::string line = "Your current average item level is " + std::to_string(ctx.avgItemLevel) + ".";

    if (!ctx.inGroup)
        line += " You are not currently in a group.";
    else if (ctx.isGroupLeader)
        line += " You are the leader of your current group.";
    else
        line += " You are in a group right now, but you are not its leader.";

    if (ctx.inInstance && !ctx.instanceName.empty())
        line += " You are currently inside " + ctx.instanceName + ".";
    else
        line += " You are not currently inside a dungeon or raid instance.";

    uint32_t gold   = ctx.goldCopper / kCopperPerGold;
    uint32_t silver = (ctx.goldCopper / kCopperPerSilver) % kCopperPerSilver;
    line += " You currently have " + std::to_string(gold) + " gold and " + std::to_string(silver) + " silver.";

    if (!ctx.zoneName.empty())
        line += " You are currently in " + ctx.zoneName + ".";

    return line;
}
