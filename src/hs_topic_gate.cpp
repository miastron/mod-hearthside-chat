#include "hs_topic_gate.h"

namespace
{
    // Player::GetMoney() is copper; 1g = 10000c. Whole gold only: silver
    // changed on nearly every loot pickup, and any change here invalidates
    // the backend's prompt cache for everything after this line.
    constexpr uint32_t kCopperPerGold = 10000;
}

// Terse on purpose (2026-10-05): this line is prefilled on every reply and
// differs per bot, so it is never served from the prompt cache. The full
// sentences it replaced cost 47 tokens; this costs ~26 with the same facts,
// negatives included. Change it only together with
// Claude/finetune/add_context_layers.py's topic_gate_line.
std::string Hs_TopicGateLine(const HsTopicGateContext& ctx)
{
    std::string line = "Item level " + std::to_string(ctx.avgItemLevel) + ".";

    if (!ctx.inGroup)
        line += " Not in a group.";
    else if (ctx.isGroupLeader)
        line += " Leading a group.";
    else
        line += " In a group, not its leader.";

    if (ctx.inInstance && !ctx.instanceName.empty())
        line += " Inside " + ctx.instanceName + ".";
    else
        line += " Not in a dungeon or raid.";

    line += " " + std::to_string(ctx.goldCopper / kCopperPerGold) + " gold.";

    if (!ctx.zoneName.empty())
        line += " In " + ctx.zoneName + ".";

    return line;
}
