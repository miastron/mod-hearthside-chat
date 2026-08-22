#ifndef MOD_HS_METRICS_H
#define MOD_HS_METRICS_H

#include <cstdint>
#include <string>
#include <vector>

// PLAN.md §4.19 / §7 step 19 -- the rolling metrics table. "In-memory
// counters reset on worldserver restart, and every question above is a
// *trend* question." Samples the observable surface this module already
// has (every counter/query behind `.hearthside status` as of step 18) into
// `hside_metrics` on an interval, so a restart doesn't erase history and a
// dashboard can be stateless. See the schema file
// (data/sql/db-characters/updates/2026_08_21_04.sql) for why this is a
// proof-of-concept slice of §4.19's fuller metric list rather than all of
// it.

// §6: "~5 minutes and a bounded ring buffer is the starting shape; the
// retention that actually answers a trend question is not known yet."
// Compiled constants, not config -- same footing as every other §6 number
// this module has shipped as a placeholder rather than an operator knob
// nobody has grounds to tune yet.
constexpr uint32_t kHsMetricsSampleIntervalSeconds = 300;
constexpr uint32_t kHsMetricsRetentionDays         = 7;

// Takes one sample and inserts it, then prunes rows older than
// kHsMetricsRetentionDays -- the "bounded ring buffer" half of §4.19's
// design, done as a delete-old-rows sweep rather than a fixed-size table
// (simpler, and the interval/retention pair already bounds row count to a
// known maximum). Called from hs_main.cpp's HsMetricsWorldScript.
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
