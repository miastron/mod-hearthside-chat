#ifndef MOD_HS_PROXIMITY_H
#define MOD_HS_PROXIMITY_H

#include "Player.h"

#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

// Bucketing for the module's "who is standing near whom" scans.
//
// Four surfaces walk the realm population every ~30s looking for pairs within
// Say.Distance of each other: hs_ambient.cpp's /say scan, hs_opener.cpp's
// proximity openers, hs_script.cpp's near-player scene trigger, and the same
// file's channel-script cast. Done naively each is O(real players x bots)
// with an IsWithinDistInMap call per pair -- review G3 measured ~110k of them
// per tick at the 334 bots this realm runs, ~1M at 1000 -- and only
// hs_ambient.cpp was ever fixed. Review item 13 is the other two.
//
// The narrowing is exact, not an approximation. IsWithinDistInMap is false
// across maps; every one of these scans already requires the same team; and
// two players in different zones cannot be within Say.Distance (20 yards) of
// each other. So the bucket for a player's own {map, zone, team} contains
// every possible match, and the per-player walk shrinks to "whoever is
// standing in the same zone". Phase is still decided by IsWithinDistInMap
// itself, exactly as before: this only narrows which pairs it is asked about.
namespace HsProximity
{
    using Cell = std::tuple<uint32_t, uint32_t, uint8_t>; // map, zone, team

    inline Cell CellOf(Player* p)
    {
        return Cell{ p->GetMapId(), p->GetZoneId(), static_cast<uint8_t>(p->GetTeamId()) };
    }

    using Index = std::map<Cell, std::vector<Player*>>;

    inline Index BucketByCell(const std::vector<Player*>& players)
    {
        Index index;
        for (Player* p : players)
            index[CellOf(p)].push_back(p);
        return index;
    }

    // The bucket `p` itself falls in, or an empty one. Returned by reference:
    // callers walk it in place, and the empty case must not allocate on every
    // miss (most zones hold no bots at all).
    inline const std::vector<Player*>& InSameCell(const Index& index, Player* p)
    {
        static const std::vector<Player*> kEmpty;
        auto it = index.find(CellOf(p));
        return it == index.end() ? kEmpty : it->second;
    }
}

#endif // MOD_HS_PROXIMITY_H
