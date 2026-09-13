#ifndef MOD_HS_HANDLER_H
#define MOD_HS_HANDLER_H

#include "ScriptMgr.h"
#include <string>

// Chat hooks. /say, party/raid, and guild all gather real-player-gated
// candidates and hand them to the arbiter (hs_arbiter.h); whisper is the one
// 1:1 surface that skips the arbiter, since there's only ever one candidate.
// All route through the tier-ceiling check and admission (hs_queue.h) rather
// than dispatching an LLM call directly. The Channel* overload (§4.17) is
// corpus-only. It never routes through TryDispatch/Hs_TryEnqueue at all; see
// TryChannelCorpusReply.
class HsChatHandler : public PlayerScript
{
public:
    HsChatHandler() : PlayerScript("HsChatHandler", {
        PLAYERHOOK_CAN_PLAYER_USE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT,
    }) {}

    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg) override;
    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Player* receiver) override;
    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Group* group) override;
    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Guild* guild) override;
    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel) override;
};

// Drains the delivery queue once per world tick. All the actual work now
// lives in hs_queue.cpp; this just calls Hs_DeliverPending() from the world
// thread. Within the reactive pipeline that is the only place a Player*/
// PlayerbotAI* may be touched -- the scope hs_queue.h states precisely. It
// is not a module-wide claim: hs_script.cpp's turn delivery, hs_ambient.cpp,
// hs_opener.cpp and the identity sweep touch players directly too, each from
// the world thread, which is what actually makes any of it safe.
class HsDeliveryWorldScript : public WorldScript
{
public:
    HsDeliveryWorldScript() : WorldScript("HsDeliveryWorldScript") {}
    void OnUpdate(uint32_t diff) override;
};

#endif // MOD_HS_HANDLER_H
