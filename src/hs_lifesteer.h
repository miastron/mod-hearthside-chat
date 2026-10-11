#ifndef MOD_HS_LIFESTEER_H
#define MOD_HS_LIFESTEER_H

#include "hs_hash.h"

#include <cstdint>

// Archetype steering for the third-party mod-playerbots-city-life and
// mod-playerbots-pvp-life modules, without modifying either of them.
//
// Both modules borrow random bots for their activities and share one
// in-memory claim registry (their LifeBotReservation.h) so they never take
// each other's bots. A claim blocks a bot from *both*, and neither module
// offers a per-module filter, so steering is done from outside with two
// tools (hs_lifesteer.cpp):
//
//   1. Rotating claims. Each hour, ClaimPercent of the bots whose archetype
//      is not on the PvP list are claimed (under the registry's unused
//      Artisans owner slot), which tilts PvP Life's candidate pool toward
//      PvP/troll archetypes. Rotation keeps every archetype in the cities
//      some of the time.
//   2. Majority releases. Whenever PvP Life holds more misfit archetypes than
//      fitting ones, the surplus misfits are handed back through its public
//      DisengageParticipant() and claimed for a cooldown so they are not
//      drawn again straight away.
//
// Like the experience ring, this never makes a bot do anything: it only
// decides which bots the Life modules may borrow. City Life is never released
// from; it only sees the rotating claims.
//
// This header is the pure half (no AzerothCore includes, standalone-testable
// in Tests/). The registry/WorldScript half compiles to nothing unless
// mod-playerbots-pvp-life's headers are on the include path.

constexpr uint64_t kHsLifeClaimSalt = 0x4C69666553746572ULL; // "LifeSter"

// True when this bot is in the claimed share for rotation window `window`
// (game time / rotation length). Independent per window, so roughly
// `percent`% of bots are claimed in any hour and a different subset the next.
inline bool Hs_IsLifeRotationClaim(uint64_t botGuid, uint64_t window, uint32_t percent)
{
    if (percent == 0)
        return false;
    if (percent >= 100)
        return true;
    uint64_t h = HsHash::Hs_MixBits64(HsHash::Hs_MixBits64(botGuid ^ kHsLifeClaimSalt) + window);
    return (h % 100) < percent;
}

// How many misfit participants to release so they do not outnumber the
// fitting ones. A majority target, not a purity one: releasing every misfit
// would cost several releases per kept bot.
inline uint32_t Hs_LifeReleaseCount(uint32_t fitting, uint32_t misfit)
{
    return misfit > fitting ? misfit - fitting : 0;
}

// Registers the steering WorldScript (hs_lifesteer.cpp). A no-op when
// mod-playerbots-pvp-life is not part of the build.
void Hs_AddLifeSteerScripts();

#endif // MOD_HS_LIFESTEER_H
