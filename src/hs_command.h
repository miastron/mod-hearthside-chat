#ifndef MOD_HS_COMMAND_H
#define MOD_HS_COMMAND_H

#include "ScriptMgr.h"
#include "CommandScript.h"

// PLAN.md §4.19: "GM commands mirror the read half so an operator in-world
// never depends on a frontend existing." `.hearthside status` works with no
// HTTP server running at all; the mutating subcommands added in step 19
// (promote/demote/retire/pin/unpin/evict-run) are the same actions the
// HTTP control API (hs_http_server.h) exposes, reachable from the console
// even if HttpServerPort is 0.
class HsCommandScript : public CommandScript
{
public:
    HsCommandScript();
    Acore::ChatCommands::ChatCommandTable GetCommands() const override;
};

#endif // MOD_HS_COMMAND_H
