#ifndef MOD_HS_PRUNE_H
#define MOD_HS_PRUNE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

// Opportunistic staleness pruning for the module's `key -> last time` maps.
//
// Each such map is only ever read through a window (a cooldown, a sighting
// decay, a proximity streak), so past it an old entry answers exactly as a
// missing one would and is pure retention. The prune runs inside a write
// path that already holds the map's mutex (no timer, no second lock), and is
// skipped below `pruneAboveSize`, so a small realm never walks the map.
// Callers' staleness constants err late on purpose: pruning early would
// change behavior, pruning late only delays reclamation.

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

// A thread-safe "has this key's cooldown passed" map: Ready() answers it,
// Mark() starts it and prunes entries four windows old. `Map` is any
// key -> Clock::time_point map; a std::map for a key with no hash (a pair).
template <typename Key, typename Map = std::unordered_map<Key, HsPrune::Clock::time_point>>
class HsCooldownMap
{
public:
    bool Ready(Key const& key, int64_t cooldownSeconds)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _map.find(key);
        return it == _map.end() ||
               HsPrune::Clock::now() - it->second >= std::chrono::seconds(cooldownSeconds);
    }

    void Mark(Key const& key, int64_t cooldownSeconds)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto now = HsPrune::Clock::now();
        _map[key] = now;
        HsPrune::PruneStale(_map, now, cooldownSeconds * 4, /*pruneAboveSize=*/512);
    }

private:
    std::mutex _mutex;
    Map        _map;
};

#endif // MOD_HS_PRUNE_H
