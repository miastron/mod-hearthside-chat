#ifndef MOD_HS_BOT_H
#define MOD_HS_BOT_H

class Player;

// One place that knows how to tell a playerbot from a human, and which bots
// this module is allowed to speak through.
//
// Same consolidation as hs_locale.h, for the same reason. `IsBot` used to be
// re-declared verbatim in eight anonymous namespaces (hs_ambient, hs_botchain,
// hs_bridge, hs_event, hs_handler, hs_memory_store, hs_opener, hs_script) and
// `IsEligibleBot` in three of those, every copy byte-for-byte identical. Eight
// copies of a predicate is eight places to update when mod-playerbots changes
// how a bot is recognized, and nothing makes a divergence visible: each copy
// compiles fine on its own.
//
// Not header-only: recognizing a bot needs PlayerbotsMgr/PlayerbotAI, and this
// header is included by files whose standalone Tests/ harnesses must stay free
// of AzerothCore dependencies.

// True if this player is driven by mod-playerbots rather than a human client.
// Null-safe.
bool Hs_IsBot(Player* p);

// True if this player is a bot that HearthsideChat.ExcludeNames does not keep
// out of the module entirely ("no reflex, grounded, corpus, or reactive reply,
// ever" -- hs_config.h). Null-safe.
//
// Deliberately a second predicate rather than folded into Hs_IsBot, and both
// are live. A scan choosing a *speaker* wants this one: an ambient line, an
// opener and a scripted turn are all corpus content spoken unprompted, so an
// excluded bot may never be the speaker. A scan asking "is a bot present"
// wants the bare Hs_IsBot instead -- an excluded bot is still a bot, and
// collapsing the two would make it register as the *human* side of a pair
// (hs_opener.cpp) or as the audience a scene is worth performing for
// (hs_script.cpp), firing a line at a bot the operator excluded.
bool Hs_IsEligibleBot(Player* p);

#endif // MOD_HS_BOT_H
