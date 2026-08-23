#ifndef MOD_HS_METRICS_H
#define MOD_HS_METRICS_H

#include <cstdint>
#include <string>
#include <vector>

// The rolling metrics table. Samples the observable surface this module
// already exposes (every counter/query behind `.hearthside status`, plus
// latency percentiles, prompt length by identity ring, and archetype/
// channel reply rates -- §4.19) into `hside_metrics`/`hside_metrics_breakdown`
// on an interval, so a restart doesn't erase history and a dashboard can be
// stateless.

// Compiled constants, not config -- a placeholder value rather than an
// operator knob, since there's no basis yet to make it tunable.
constexpr uint32_t kHsMetricsSampleIntervalSeconds = 300;
constexpr uint32_t kHsMetricsRetentionDays         = 7;

// Takes one sample and inserts it, then prunes rows older than
// kHsMetricsRetentionDays -- a delete-old-rows sweep rather than a
// fixed-size table, since the interval/retention pair already bounds row
// count to a known maximum. Called from hs_main.cpp's HsMetricsWorldScript.
void Hs_SampleMetrics();

struct HsMetricsSample
{
    std::string sampledAt;
    bool        backendDown;
    uint32_t    queueDepth;
    uint32_t    corpusRowCount;
    uint32_t    corpusRowsAddedSession;
    uint32_t    corpusRowsEvictedSession;
    uint32_t    scriptReserveDepth;
    uint32_t    scriptActiveRuns;
    uint32_t    scriptConsumed24h;
    uint32_t    identityRowCount;
    uint32_t    cardActiveCount;
    uint32_t    promotionsSession;
    uint32_t    demotionsSession;
    uint32_t    retirementsSession;
    uint32_t    memoryRowCount;
    uint32_t    openersFiredSession;

    // §4.19: reactive-tier latency percentiles (rolling window, hs_queue.h's
    // Hs_ReactiveLatencyPercentiles) and mean assembled-prompt length by
    // identity ring (hs_queue.h's Hs_PromptCharsByRing). Archetype/channel
    // reply rates don't fit this flat shape -- see hside_metrics_breakdown
    // and Hs_RecentMetricsBreakdown below instead.
    uint32_t    latencyP50Ms;
    uint32_t    latencyP95Ms;
    uint32_t    latencyP99Ms;
    uint32_t    promptCharsRing1Mean;
    uint32_t    promptCharsRing2Mean;
    uint32_t    promptCharsRing3Mean;
};

// Most-recent samples first, capped at `limit`. Backs the HTTP
// GET /api/metrics route -- there is no GM-command equivalent since a
// scrolling table of ~16 columns doesn't fit a chat window usefully;
// `.hearthside status` already gives the current-moment view of the same
// counters.
std::vector<HsMetricsSample> Hs_RecentMetrics(uint32_t limit);

// One row per (dimension, key) pair sampled alongside HsMetricsSample --
// dimension is "archetype" or "channel", key is the archetype enum name or
// Hs_ReplyChannelName(channel). Its own table (hside_metrics_breakdown)
// rather than columns on hside_metrics, since archetype/channel counts are
// variable-cardinality and don't fit that table's flat per-interval row.
struct HsMetricsBreakdownRow
{
    std::string sampledAt;
    std::string dimension;
    std::string key;
    uint32_t    repliedCount;
    uint32_t    silentCount;
};

// Most-recent samples first, capped at `limit` rows (not `limit` intervals --
// each interval contributes one row per archetype plus one per channel).
std::vector<HsMetricsBreakdownRow> Hs_RecentMetricsBreakdown(uint32_t limit);

#endif // MOD_HS_METRICS_H
