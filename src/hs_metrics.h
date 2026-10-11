#ifndef MOD_HS_METRICS_H
#define MOD_HS_METRICS_H

#include <cstdint>
#include <string>
#include <vector>

// The rolling metrics table. Samples the observable surface this module
// already exposes (every counter/query behind `.hearthside status`, plus
// latency percentiles, prompt length by identity ring, and archetype/
// channel reply rates, §4.19) into `hside_metrics`/`hside_metrics_breakdown`
// on an interval, so a restart doesn't erase history and a dashboard can be
// stateless.

// Compiled constants, not config: placeholder values rather than operator
// knobs, since there's no basis yet to make them tunable.
constexpr uint32_t kHsMetricsSampleIntervalSeconds = 300;
constexpr uint32_t kHsMetricsRetentionDays         = 7;

// Takes one sample and inserts it, then prunes rows older than
// kHsMetricsRetentionDays (a delete-old-rows sweep rather than a
// fixed-size table, since the interval/retention pair already bounds row
// count to a known maximum). Called from hs_main.cpp's HsMetricsWorldScript.
void Hs_SampleMetrics();

// hside_metrics_breakdown's counters are session-cumulative: each sample
// rewrites a running total since worldserver start, so a reader wanting
// per-interval rates diffs consecutive rows and treats a decrease as a
// restart. For dimension "channel_bucket" the two counters are granted and
// denied rate-limit takes rather than replied/silent. The reader is
// acore-web-reg's hearthside.php.

#endif // MOD_HS_METRICS_H
