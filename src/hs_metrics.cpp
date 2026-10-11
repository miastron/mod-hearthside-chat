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
    // accessors still exist for `.hearthside status`,
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
    // (see hs_metrics.h).
    // Review item 16: one multi-row INSERT for all three dimensions, not one
    // statement per row. This is ~20 rows every sampling interval, and the
    // six scalar counters above were already consolidated into a single
    // Execute for exactly this reason -- the consolidation just never reached
    // the breakdown loops.
    std::string values;
    auto appendValue = [&values](const char* dimension, const std::string& key,
                                  uint32_t repliedCount, uint32_t silentCount)
    {
        if (!values.empty())
            values += ",";
        values += "('";
        values += dimension;
        values += "','" + key + "'," + std::to_string(repliedCount) + "," + std::to_string(silentCount) + ")";
    };

    for (auto const& row : Hs_ArchetypeReplyCountsSnapshot())
    {
        // The only one of the three that needs escaping: archetype names come
        // from the hside_archetype table. The channel names below are fixed
        // strings from this module's own enums.
        std::string escapedName = row.enumName;
        CharacterDatabase.EscapeString(escapedName);
        appendValue("archetype", escapedName, row.repliedCount, row.silentCount);
    }
    for (auto const& row : Hs_ChannelReplyCountsSnapshot())
        appendValue("channel", Hs_ReplyChannelName(row.channel), row.repliedCount, row.silentCount);
    for (auto const& row : Hs_ChannelBucketSaturationSnapshot())
        appendValue("channel_bucket", Hs_ChannelKindName(row.kind), row.grantedCount, row.deniedCount);

    if (!values.empty())
        CharacterDatabase.Execute(
            "INSERT INTO hside_metrics_breakdown (dimension, dim_key, replied_count, silent_count) VALUES {}",
            values);

    CharacterDatabase.Execute(
        "DELETE FROM hside_metrics_breakdown WHERE sampled_at < NOW() - INTERVAL {} DAY", kHsMetricsRetentionDays);
}
