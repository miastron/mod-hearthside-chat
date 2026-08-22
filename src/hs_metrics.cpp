#include "hs_metrics.h"
#include "hs_generator.h"
#include "hs_identity_store.h"
#include "hs_memory_store.h"
#include "hs_opener.h"
#include "hs_queue.h"
#include "hs_script.h"

#include "DatabaseEnv.h"
#include "QueryResult.h"

namespace
{
    uint32_t CorpusRowCount()
    {
        QueryResult result = CharacterDatabase.Query("SELECT COUNT(*) FROM hside_corpus");
        return result ? (*result)[0].Get<uint32_t>() : 0;
    }
}

void Hs_SampleMetrics()
{
    CharacterDatabase.Execute(
        "INSERT INTO hside_metrics (backend_down, queue_depth, corpus_row_count, "
        "corpus_rows_added_session, corpus_rows_evicted_session, script_reserve_depth, "
        "script_active_runs, script_consumed_24h, identity_row_count, card_active_count, "
        "promotions_session, demotions_session, retirements_session, memory_row_count, "
        "openers_fired_session) VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
        Hs_IsBackendDown() ? 1 : 0, Hs_PendingQueueDepth(), CorpusRowCount(),
        Hs_GeneratorRowsAddedThisSession(), Hs_RowsEvictedThisSession(), Hs_ScriptReserveDepth(),
        Hs_ActiveScriptRunCount(), Hs_ScriptsConsumedLast24h(), Hs_IdentityRowCount(), Hs_CardActiveCount(),
        Hs_PromotionsThisSession(), Hs_DemotionsThisSession(), Hs_RetirementsThisSession(), Hs_MemoryRowCount(),
        Hs_OpenersFiredThisSession());

    CharacterDatabase.Execute(
        "DELETE FROM hside_metrics WHERE sampled_at < NOW() - INTERVAL {} DAY", kHsMetricsRetentionDays);
}

std::vector<HsMetricsSample> Hs_RecentMetrics(uint32_t limit)
{
    std::vector<HsMetricsSample> samples;

    QueryResult result = CharacterDatabase.Query(
        "SELECT sampled_at, backend_down, queue_depth, corpus_row_count, corpus_rows_added_session, "
        "corpus_rows_evicted_session, script_reserve_depth, script_active_runs, script_consumed_24h, "
        "identity_row_count, card_active_count, promotions_session, demotions_session, "
        "retirements_session, memory_row_count, openers_fired_session "
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
        samples.push_back(s);
    } while (result->NextRow());

    return samples;
}
