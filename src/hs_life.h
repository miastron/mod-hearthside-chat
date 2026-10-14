#ifndef MOD_HS_LIFE_H
#define MOD_HS_LIFE_H

#include <cstdint>

// The bridge to mod-hearthside-life, the coordinator City and PvP share.
// Everything Chat does with Life goes through here, and hs_life.cpp only
// compiles the Life side when ../../mod-hearthside-life/src/ is beside this
// module (__has_include). Without it every call below is a no-op and Chat
// builds and behaves exactly as before Life existed.
//
// HearthsideChat.Life.Enable (g_HsLifeEnable) switches the bridge off at
// runtime without a rebuild: City and PvP then see no archetype provider
// (every bot fits their filters) and no busy marks, as on a server without
// Chat. HearthsideChat.Enable = 0 does the same.

// Hands Life the archetype provider and the event listener. From
// Addmod_hearthside_chatScripts, before any module reads its config.
//
// The listener only queues: a CrowdPair (City stood two residents together
// near a real player) becomes an offer to the /say script runner
// (Hs_OfferScriptPair), which tries it on its own tick. RaidDefenseStarted
// is dropped until it has dataset rows and an event type of its own.
void Hs_RegisterWithLife();

// True when Life is in this build and the bridge is switched on.
bool Hs_LifeActive();

// A bot cast in a /say scene is busy for durationMs (an upper bound: the
// scene clears it when it ends or aborts). City and PvP leave a busy bot
// where it is.
void Hs_LifeMarkBusy(uint64_t botGuid, uint32_t durationMs);
void Hs_LifeClearBusy(uint64_t botGuid);

#endif // MOD_HS_LIFE_H
