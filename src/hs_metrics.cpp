#include "hs_metrics.h"
#include "hs_generator.h"
#include "hs_identity_store.h"
#include "hs_memory_store.h"
#include "hs_opener.h"
#include "hs_queue.h"
#include "hs_script.h"

#include "DatabaseEnv.h"
#include "QueryResult.h"


void Hs_SampleMetrics()
{
    HsLatencyPercentiles     latency      = Hs_ReactiveLatencyPercentiles();
    HsPromptCharsByRing      promptByRing = Hs_PromptCharsByRing();
    HsTtlDropStats           ttlDrops     = Hs_TtlDropStatsSnapshot();
    HsBucketSaturationStats  bucketSat    = Hs_GlobalBucketSaturationSnapshot();

    // Review G6: the six DB-sourced counters (corpus rows, /say script
    // reserve, scripts consumed in 24h, identity rows, card-active rows,
    // memory rows) used to be six *synchronous* Query() calls evaluated as
    // arguments to this Execute -- six blocking round trips on the world
    // thread every 300s, serialized against everything else using
    // CharacterDatabase's single synchronous connection.
    //
    // They are now scalar subqueries inside the INSERT itself, so this whole
    // function makes zero synchronous reads: one async Execute that the
    // worker evaluates on its own connection. The in-memory counters
    // (session totals, latency percentiles, queue depth) stay as
    // format arguments -- they are atomics and mutex reads, not DB work.
    //
    // The subqueries are the exact statements CorpusRowCount /
    // Hs_ScriptReserveDepth / Hs_ScriptsConsumedLast24h /
    // Hs_IdentityRowCount / Hs_CardActiveCount / Hs_MemoryRowCount ran; those
    // accessors still exist for `.hearthside status` and the HTTP routes,
    // where a caller genuinely wants the value back.
    CharacterDatabase.Execute(
        "INSERT INTO hside_metrics (backend_down, queue_depth, corpus_row_count, "
        "corpus_rows_added_session, corpus_rows_evicted_session, script_reserve_depth, "
        "script_active_runs, script_consumed_24h, identity_row_count, card_active_count, "
        "promotions_session, demotions_session, retirements_session, memory_row_count, "
        "openers_fired_session, latency_p50_ms, latency_p95_ms, latency_p99_ms, "
        "prompt_chars_ring1_mean, prompt_chars_ring2_mean, prompt_chars_ring3_mean, "
        "ttl_dropped_session, ttl_processed_session, bucket_denied_session, bucket_attempted_session) "
        "SELECT {}, {}, "
        "(SELECT COUNT(*) FROM hside_corpus), "
        "{}, {}, "
        "(SELECT COUNT(*) FROM hside_script WHERE consumed_at IS NULL AND channel IS NULL), "
        "{}, "
        "(SELECT COUNT(*) FROM hside_script WHERE consumed_at IS NOT NULL AND consumed_at >= NOW() - INTERVAL 1 DAY), "
        "(SELECT COUNT(*) FROM hside_identity), "
        "(SELECT COUNT(*) FROM hside_identity WHERE card_active = 1), "
        "{}, {}, {}, "
        "(SELECT COUNT(*) FROM hside_memory), "
        "{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}",
        Hs_IsBackendDown() ? 1 : 0, Hs_PendingQueueDepth(),
        Hs_GeneratorRowsAddedThisSession(), Hs_RowsEvictedThisSession(),
        Hs_ActiveScriptRunCount(),
        Hs_PromotionsThisSession(), Hs_DemotionsThisSession(), Hs_RetirementsThisSession(),
        Hs_OpenersFiredThisSession(), latency.p50Ms, latency.p95Ms, latency.p99Ms,
        promptByRing.ring1Mean, promptByRing.ring2Mean, promptByRing.ring3Mean,
        ttlDrops.droppedCount, ttlDrops.processedCount, bucketSat.deniedCount, bucketSat.attemptCount);

    CharacterDatabase.Execute(
        "DELETE FROM hside_metrics WHERE sampled_at < NOW() - INTERVAL {} DAY", kHsMetricsRetentionDays);

    // Per-archetype and per-channel reply-vs-silence counts, one row each:
    // variable cardinality, doesn't fit hside_metrics' flat per-interval row
    // (see hs_metrics.h's HsMetricsBreakdownRow doc comment).
    for (auto const& row : Hs_ArchetypeReplyCountsSnapshot())
    {
        std::string escapedName = row.enumName;
        CharacterDatabase.EscapeString(escapedName);
        CharacterDatabase.Execute(
            "INSERT INTO hside_metrics_breakdown (dimension, dim_key, replied_count, silent_count) "
            "VALUES ('archetype', '{}', {}, {})",
            escapedName, row.repliedCount, row.silentCount);
    }
    for (auto const& row : Hs_ChannelReplyCountsSnapshot())
    {
        CharacterDatabase.Execute(
            "INSERT INTO hside_metrics_breakdown (dimension, dim_key, replied_count, silent_count) "
            "VALUES ('channel', '{}', {}, {})",
            Hs_ReplyChannelName(row.channel), row.repliedCount, row.silentCount);
    }
    for (auto const& row : Hs_ChannelBucketSaturationSnapshot())
    {
        CharacterDatabase.Execute(
            "INSERT INTO hside_metrics_breakdown (dimension, dim_key, replied_count, silent_count) "
            "VALUES ('channel_bucket', '{}', {}, {})",
            Hs_ChannelKindName(row.kind), row.grantedCount, row.deniedCount);
    }

    CharacterDatabase.Execute(
        "DELETE FROM hside_metrics_breakdown WHERE sampled_at < NOW() - INTERVAL {} DAY", kHsMetricsRetentionDays);
}

std::vector<HsMetricsSample> Hs_RecentMetrics(uint32_t limit)
{
    std::vector<HsMetricsSample> samples;

    QueryResult result = CharacterDatabase.Query(
        "SELECT sampled_at, backend_down, queue_depth, corpus_row_count, corpus_rows_added_session, "
        "corpus_rows_evicted_session, script_reserve_depth, script_active_runs, script_consumed_24h, "
        "identity_row_count, card_active_count, promotions_session, demotions_session, "
        "retirements_session, memory_row_count, openers_fired_session, latency_p50_ms, latency_p95_ms, "
        "latency_p99_ms, prompt_chars_ring1_mean, prompt_chars_ring2_mean, prompt_chars_ring3_mean, "
        "ttl_dropped_session, ttl_processed_session, bucket_denied_session, bucket_attempted_session "
        "FROM hside_metrics ORDER BY sampled_at DESC LIMIT {}", limit);
    if (!result)
        return samples;

    do
    {
        HsMetricsSample s;
        s.sampledAt                = (*result)[0].Get<std::string>();
        s.backendDown               = (*result)[1].Get<uint8_t>() != 0;
        s.queueDepth                = (*result)[2].Get<uint32_t>();
        s.corpusRowCount            = (*result)[3].Get<uint32_t>();
        s.corpusRowsAddedSession   = (*result)[4].Get<uint32_t>();
        s.corpusRowsEvictedSession = (*result)[5].Get<uint32_t>();
        s.scriptReserveDepth       = (*result)[6].Get<uint32_t>();
        s.scriptActiveRuns         = (*result)[7].Get<uint32_t>();
        s.scriptConsumed24h        = (*result)[8].Get<uint32_t>();
        s.identityRowCount         = (*result)[9].Get<uint32_t>();
        s.cardActiveCount          = (*result)[10].Get<uint32_t>();
        s.promotionsSession        = (*result)[11].Get<uint32_t>();
        s.demotionsSession         = (*result)[12].Get<uint32_t>();
        s.retirementsSession       = (*result)[13].Get<uint32_t>();
        s.memoryRowCount           = (*result)[14].Get<uint32_t>();
        s.openersFiredSession      = (*result)[15].Get<uint32_t>();
        s.latencyP50Ms              = (*result)[16].Get<uint32_t>();
        s.latencyP95Ms              = (*result)[17].Get<uint32_t>();
        s.latencyP99Ms              = (*result)[18].Get<uint32_t>();
        s.promptCharsRing1Mean     = (*result)[19].Get<uint32_t>();
        s.promptCharsRing2Mean     = (*result)[20].Get<uint32_t>();
        s.promptCharsRing3Mean     = (*result)[21].Get<uint32_t>();
        s.ttlDroppedSession        = (*result)[22].Get<uint64_t>();
        s.ttlProcessedSession      = (*result)[23].Get<uint64_t>();
        s.bucketDeniedSession      = (*result)[24].Get<uint64_t>();
        s.bucketAttemptedSession  = (*result)[25].Get<uint64_t>();
        samples.push_back(s);
    } while (result->NextRow());

    return samples;
}

std::vector<HsMetricsBreakdownRow> Hs_RecentMetricsBreakdown(uint32_t limit)
{
    std::vector<HsMetricsBreakdownRow> rows;

    QueryResult result = CharacterDatabase.Query(
        "SELECT sampled_at, dimension, dim_key, replied_count, silent_count "
        "FROM hside_metrics_breakdown ORDER BY sampled_at DESC LIMIT {}", limit);
    if (!result)
        return rows;

    do
    {
        HsMetricsBreakdownRow r;
        r.sampledAt    = (*result)[0].Get<std::string>();
        r.dimension    = (*result)[1].Get<std::string>();
        r.key           = (*result)[2].Get<std::string>();
        r.repliedCount = (*result)[3].Get<uint32_t>();
        r.silentCount  = (*result)[4].Get<uint32_t>();
        rows.push_back(r);
    } while (result->NextRow());

    return rows;
}
