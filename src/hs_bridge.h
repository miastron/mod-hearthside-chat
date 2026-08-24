#ifndef MOD_HS_BRIDGE_H
#define MOD_HS_BRIDGE_H

#include "ScriptMgr.h"

// Read-only addon-message bridge for the HearthsideInspect client addon.
// Answers one request type -- GET~INSPECT~<botName>~<token>, sent as a
// self-whisper addon message the same way mod-multibot-bridge's MBOT
// protocol does (SendAddonMessage(prefix, msg, "WHISPER", ownName) reaches
// the server as a whisper-to-self whose body starts with "HSI\t", which the
// client never displays -- only delivers back as CHAT_MSG_ADDON) -- with
// the bot's live archetype/personality/memory-flavor data. This is a third
// transport for the same HsIdentityInspection data `.hearthside inspect`
// (hs_command.cpp) and the HTTP inspect route (hs_http_server.cpp) already
// expose, not a new data source, so there is no separate permission gate:
// any player can inspect any bot's Hearthside tab, the same trust level as
// the rest of Blizzard's own Inspect frame.
//
// No HELLO/capability handshake -- the protocol is a single request/
// response pair, so an addon talking to an old or missing bridge just times
// out client-side rather than getting stuck tracking connection state.
class HsBridgePlayerScript : public PlayerScript
{
public:
    HsBridgePlayerScript() : PlayerScript("HsBridgePlayerScript", { PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) {}

    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Player* receiver) override;
};

#endif // MOD_HS_BRIDGE_H
