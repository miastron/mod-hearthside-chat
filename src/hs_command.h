#ifndef MOD_HS_COMMAND_H
#define MOD_HS_COMMAND_H

#include "ScriptMgr.h"
#include "CommandScript.h"

// GM commands mirror the read half so an operator in-world never depends on
// a frontend existing. `.hearthside status` works with no HTTP server
// running at all; the mutating subcommands (promote/demote/retire/pin/
// unpin/evict-run) are the same actions the HTTP control API
// (hs_http_server.h) exposes, reachable from the console even if
// HttpServerPort is 0.
class HsCommandScript : public CommandScript
{
public:
    HsCommandScript();
    Acore::ChatCommands::ChatCommandTable GetCommands() const override;
};

#endif // MOD_HS_COMMAND_H
