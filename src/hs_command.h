#ifndef MOD_HS_COMMAND_H
#define MOD_HS_COMMAND_H

#include "ScriptMgr.h"
#include "CommandScript.h"

// GM commands: `.hearthside status` and inspect for reading; the mutating
// subcommands (promote/demote/retire/pin/unpin/
// evict-run) and `archetype` work from in-game chat and the worldserver
// console alike.
class HsCommandScript : public CommandScript
{
public:
    HsCommandScript();
    Acore::ChatCommands::ChatCommandTable GetCommands() const override;
};

#endif // MOD_HS_COMMAND_H
