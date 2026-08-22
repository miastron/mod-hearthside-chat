#ifndef MOD_HS_HANDLER_H
#define MOD_HS_HANDLER_H

#include "ScriptMgr.h"
#include <string>

// Chat hooks — PLAN.md §7 step 4: /say gathers real-player-gated candidates
// and hands them to the arbiter (hs_arbiter.h, §4.15); whisper stays a simple
// 1:1 surface. Both route the arbiter's/receiver's tier-ceiling check (§4.14)
// and admission (hs_queue.h, §4.3) rather than dispatching an LLM call
// directly. Party/raid/guild/channel surfaces are later steps.
class HsChatHandler : public PlayerScript
{
public:
    HsChatHandler() : PlayerScript("HsChatHandler", {
        PLAYERHOOK_CAN_PLAYER_USE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
    }) {}

    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg) override;
    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Player* receiver) override;
};

// Drains the delivery queue once per world tick. All the actual work now
// lives in hs_queue.cpp; this just calls Hs_DeliverPending() from the world
// thread, which is the only place a Player*/PlayerbotAI* may be touched.
class HsDeliveryWorldScript : public WorldScript
{
public:
    HsDeliveryWorldScript() : WorldScript("HsDeliveryWorldScript") {}
    void OnUpdate(uint32_t diff) override;
};

#endif // MOD_HS_HANDLER_H
