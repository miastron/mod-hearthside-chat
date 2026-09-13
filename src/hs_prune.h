#ifndef MOD_HS_PRUNE_H
#define MOD_HS_PRUNE_H

#include <chrono>
#include <cstddef>
#include <cstdint>

// Opportunistic staleness pruning for the module's per-bot and per-pair time
// maps.
//
// Several subsystems keep a `key -> last time something happened` map that is
// only ever *read* through a window: a cooldown, a sighting decay, a
// proximity streak. Past that window an old entry answers every query exactly
// as a missing one would, so it is pure retention with no behavioral effect,
// and on a long-uptime realm it accumulates for the life of the process.
//
// This is the shape hs_botchain.cpp's PruneStaleLocked already uses, lifted
// so the other call sites do not each re-derive it:
//
//   - **opportunistic, not timed.** The prune runs inside a write path that
//     already holds the map's mutex, so it needs no scan, no WorldScript, and
//     no second lock acquisition.
//   - **size-gated.** Below `pruneAboveSize` the walk is skipped entirely, so
//     a small realm never pays for it; the cost only appears once the map is
//     big enough for the cost to be worth paying.
//
// Every caller's constants are deliberately generous relative to the window
// the map is actually read through. Pruning early would change behavior;
// pruning late only delays reclamation, so the constants err late.
//
// Two shapes make a map *worth* pruning: a key that is a *pair* (whose count
// is the product of two populations, not the sum), or a value holding more
// than a timestamp. hs_queue.cpp's g_History is the first; g_RecentUtterances
// and hs_experience.cpp's ring are the second (bot-keyed, but each
// accumulates content). hs_experience_store.cpp's g_LastZoneByBot is the
// counter-example left unpruned on purpose: bot-keyed, one id per entry.
//
// It is applied more widely than those three, though (this comment claimed
// "three call sites today" until review item 12; there are ten). The rest are
// plain bot- or player-keyed cooldown maps -- hs_ambient's g_LastAmbientAt,
// hs_opener's g_LastOpenerAt, hs_script's g_LastWitnessAt, hs_style's
// g_LastTradeSighting, hs_queue's three g_Last*At maps -- which the criteria
// above would call exempt. Pruning them anyway is deliberate and free: the
// call sits in a write path that already holds the mutex, the size gate means
// a small realm never walks the map at all, and an entry past its cooldown
// window answers identically whether it is stale or absent. The criteria say
// which maps *must* be pruned to stay bounded, not which ones may be.

namespace HsPrune
{
    using Clock = std::chrono::steady_clock;

    // Projection for the common case where the mapped value is itself the
    // timestamp, rather than a struct carrying one.
    struct TimePointItself
    {
        Clock::time_point operator()(const Clock::time_point& t) const { return t; }
    };

    // Erases every entry whose projected timestamp is at least `staleSeconds`
    // old. No-op while the map holds `pruneAboveSize` entries or fewer.
    // The caller must already hold the map's mutex.
    template <typename Map, typename GetTime>
    void PruneStale(Map& map, Clock::time_point now, int64_t staleSeconds,
                    std::size_t pruneAboveSize, GetTime getTime)
    {
        if (map.size() <= pruneAboveSize)
            return;

        for (auto it = map.begin(); it != map.end(); )
        {
            int64_t elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - getTime(it->second)).count();
            if (elapsed >= staleSeconds)
                it = map.erase(it);
            else
                ++it;
        }
    }

    template <typename Map>
    void PruneStale(Map& map, Clock::time_point now, int64_t staleSeconds,
                    std::size_t pruneAboveSize)
    {
        PruneStale(map, now, staleSeconds, pruneAboveSize, TimePointItself{});
    }
}

#endif // MOD_HS_PRUNE_H
