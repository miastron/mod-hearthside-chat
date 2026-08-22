#ifndef MOD_HS_METRICS_H
#define MOD_HS_METRICS_H

#include <cstdint>
#include <string>
#include <vector>

// The rolling metrics table. Samples the observable surface this module
// already exposes (every counter/query behind `.hearthside status`) into
// `hside_metrics` on an interval, so a restart doesn't erase history and a
// dashboard can be stateless. This is a slice of a fuller metric set --
// see the schema file (data/sql/db-characters/updates/2026_08_21_04.sql).

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
};

// Most-recent samples first, capped at `limit`. Backs the HTTP
// GET /api/metrics route -- there is no GM-command equivalent since a
// scrolling table of ~16 columns doesn't fit a chat window usefully;
// `.hearthside status` already gives the current-moment view of the same
// counters.
std::vector<HsMetricsSample> Hs_RecentMetrics(uint32_t limit);

#endif // MOD_HS_METRICS_H
