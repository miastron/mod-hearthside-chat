#ifndef MOD_HS_COMMAND_H
#define MOD_HS_COMMAND_H

#include "ScriptMgr.h"
#include "CommandScript.h"

#include <string>

// GM commands: `.hearthside status` and inspect for reading; the mutating
// subcommands (promote/demote/retire/pin/unpin/
// evict-run) and `archetype` work from in-game chat and the worldserver
// console alike.
// Whisper `botName` as `senderName` would (the body of `.hearthside ask`),
// shared with the HTTP API's /api/ask. World thread only. Returns "" on
// success, else why not; on success `senderName` holds the stored spelling.
std::string Hs_AskBot(std::string& senderName, const std::string& botName, const std::string& text);

class HsCommandScript : public CommandScript
{
public:
    HsCommandScript();
    Acore::ChatCommands::ChatCommandTable GetCommands() const override;
};

#endif // MOD_HS_COMMAND_H
