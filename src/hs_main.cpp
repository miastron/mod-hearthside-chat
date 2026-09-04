#include "hs_ambient.h"
#include "hs_archetype_store.h"
#include "hs_bridge.h"
#include "hs_config.h"
#include "hs_engagement.h"
#include "hs_event.h"
#include "hs_event_affinity_store.h"
#include "hs_generator.h"
#include "hs_grounded_store.h"
#include "hs_handler.h"
#include "hs_command.h"
#include "hs_http_server.h"
#include "hs_identity_store.h"
#include "hs_memory_store.h"
#include "hs_metrics.h"
#include "hs_opener.h"
#include "hs_queue.h"
#include "hs_script.h"

#include "ScriptMgr.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "QueryResult.h"

#include <cstdint>

namespace
{
    // Review B4: the two once-daily sweeps below used to accumulate
    // worldserver `diff` from process start with no persistence, so on a
    // realm restarted more often than once a day the accumulator never
    // reached 86400000ms and *neither sweep ever ran* -- score decay,
    // friend-poll pinning, card demotion, level-drop retirement, orphan
    // cleanup, corpus over-quota eviction and unused-row eviction were all
    // silently dead. Nothing logged it.
    //
    // hside_sweep_state carries the last run time across restarts. The
    // per-tick accumulator stays (it is what makes the common case free);
    // these two helpers only seed it at startup and stamp it on each fire,
    // so this costs one query at boot plus one write per sweep per day.
    constexpr char const* kSweepIdentityDaily  = "identity_daily";
    constexpr char const* kSweepCorpusEviction = "corpus_eviction";

    // Milliseconds still to wait before `sweepName` is due, given the
    // persisted last-run time and the sweep's interval. Returns 0 when it is
    // due now (never run, or the interval has already elapsed while the
    // realm was down), which makes the sweep fire on the first tick.
    uint32 SweepBacklogMs(char const* sweepName, uint32 intervalMs)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT TIMESTAMPDIFF(SECOND, last_run_at, NOW()) FROM hside_sweep_state WHERE sweep_name = '{}'",
            sweepName);
        if (!result || (*result)[0].IsNull())
            return intervalMs; // never run: due immediately

        int64 elapsedSec = (*result)[0].Get<int64>();
        if (elapsedSec < 0)
            elapsedSec = 0; // clock moved backwards; treat as just-run
        uint64 elapsedMs = static_cast<uint64>(elapsedSec) * 1000ull;
        return elapsedMs >= intervalMs ? intervalMs : static_cast<uint32>(elapsedMs);
    }

    // DirectExecute, not Execute: the startup read above and this write are
    // the same read-after-write pair the rest of the module now runs
    // synchronously (review A2/A3), and a sweep stamp that has not landed
    // before the next restart's query would replay the sweep.
    void StampSweepRun(char const* sweepName)
    {
        CharacterDatabase.DirectExecute(
            "INSERT INTO hside_sweep_state (sweep_name, last_run_at) VALUES ('{}', NOW()) "
            "ON DUPLICATE KEY UPDATE last_run_at = NOW()",
            sweepName);
    }

    // Loads hside_archetype into memory so weights/care/reply/cap/talksAbout/
    // profanity are retunable without a rebuild. Registered right after
    // HsConfigWorldScript and before anything that could draw an archetype
    // (queue worker, reflex/grounded/corpus replies), all of which only run
    // once a player is in the world.
    class HsArchetypeLifecycleWorldScript : public WorldScript
    {
    public:
        HsArchetypeLifecycleWorldScript() : WorldScript("HsArchetypeLifecycleWorldScript") {}
        void OnStartup() override
        {
            Hs_LoadArchetypesFromDb();
            // Overrides validate their enum_name against the table just
            // loaded above, so this must come second. Startup-only: a
            // pin survives a `.reload config` untouched, since neither that
            // reload nor Hs_SetArchetypeTable's replace touches the
            // separate override map.
            Hs_LoadArchetypeOverridesFromDb();
        }
        void OnAfterConfigLoad(bool reload) override
        {
            // `.reload config` doesn't touch hside_archetype, but reusing it
            // to also pick up table edits avoids a second GM command.
            if (reload)
                Hs_LoadArchetypesFromDb();
        }
    };

    // Loads hside_grounded_question/hside_grounded_template into memory,
    // same "SQL is the source of truth, retunable without a rebuild" shape
    // and lifecycle as HsArchetypeLifecycleWorldScript above. Registered
    // right after it, before anything that could call
    // Hs_MatchGroundedQuestion/Hs_BuildGroundedReply (hs_handler.cpp's
    // TryGrounded, reachable only once a player is in the world).
    class HsGroundedLifecycleWorldScript : public WorldScript
    {
    public:
        HsGroundedLifecycleWorldScript() : WorldScript("HsGroundedLifecycleWorldScript") {}
        void OnStartup() override
        {
            Hs_LoadGroundedQuestionsFromDb();
            Hs_LoadGroundedTemplatesFromDb();
        }
        void OnAfterConfigLoad(bool reload) override
        {
            if (reload)
            {
                Hs_LoadGroundedQuestionsFromDb();
                Hs_LoadGroundedTemplatesFromDb();
            }
        }
    };

    // Loads hside_event_affinity (Claude/archive/PLAN-ARBITER.md §2): per-event archetype
    // weighting for the event arbiter. Same "SQL is the source of truth,
    // retunable without a rebuild" shape as the two above, and registered
    // after HsArchetypeLifecycleWorldScript specifically: the loader
    // validates each row's archetype against the live archetype table
    // (Hs_ArchetypeForName), so that table has to be populated first.
    class HsEventAffinityLifecycleWorldScript : public WorldScript
    {
    public:
        HsEventAffinityLifecycleWorldScript() : WorldScript("HsEventAffinityLifecycleWorldScript") {}
        void OnStartup() override { Hs_LoadEventAffinityFromDb(); }
        void OnAfterConfigLoad(bool reload) override
        {
            if (reload)
                Hs_LoadEventAffinityFromDb();
        }
    };

    // Starts/stops the runtime queue's worker thread. Kept separate from
    // HsConfigWorldScript so config loading stays config-only; registered
    // after it so the worker never starts before HearthsideChat.* is loaded.
    class HsQueueLifecycleWorldScript : public WorldScript
    {
    public:
        HsQueueLifecycleWorldScript() : WorldScript("HsQueueLifecycleWorldScript") {}
        void OnStartup() override { Hs_QueueStartup(); }
        void OnShutdown() override { Hs_QueueShutdown(); }
    };

    // Same lifecycle shape for the idle-time generator's background thread.
    // Kept separate from the queue's: the generator only calls into
    // Hs_IsReactiveIdle(), it doesn't share the reactive worker thread.
    class HsGeneratorLifecycleWorldScript : public WorldScript
    {
    public:
        HsGeneratorLifecycleWorldScript() : WorldScript("HsGeneratorLifecycleWorldScript") {}
        void OnStartup() override { Hs_GeneratorStartup(); }
        void OnShutdown() override { Hs_GeneratorShutdown(); }
    };

    // hside_identity is the source of truth for playerbots' exclude vectors,
    // but playerbots' own OnAfterConfigLoad handler unconditionally clears
    // both vectors on every `.reload config` (it has no way to encode these
    // names), so they need periodic reapplication rather than a one-time
    // push. Confirmed live: promoting a bot and issuing `.reload config`
    // leaves it out of both vectors until this reconcile runs again.
    constexpr uint32_t kIdentityReconcileIntervalMs = 300000;

    // Decay/pinning/retirement sweep. Shares this WorldScript rather than
    // getting its own: decay/dormancy operate on day/week-scale windows
    // (hs_identity.h's kHsScoreDecayGraceDays/kHsCardDormancyDays), so a
    // once-daily cadence fits better than the reconcile's 300s interval.
    // See Hs_RunIdentityDailySweep (hs_identity_store.h) for what it does.
    constexpr uint32_t kIdentitySweepIntervalMs = 86400000;

    class HsIdentityLifecycleWorldScript : public WorldScript
    {
    public:
        HsIdentityLifecycleWorldScript() : WorldScript("HsIdentityLifecycleWorldScript") {}
        void OnStartup() override
        {
            Hs_ApplyExcludeVectorsFromIdentityTable();
            // Review B4: carry the daily sweep's clock across the restart.
            _msSinceSweep = SweepBacklogMs(kSweepIdentityDaily, kIdentitySweepIntervalMs);
        }
        void OnAfterConfigLoad(bool reload) override
        {
            if (reload)
                Hs_ApplyExcludeVectorsFromIdentityTable();
        }
        void OnUpdate(uint32 diff) override
        {
            // Every tick: apply exclude-vector pushes/removes queued by the
            // generator, queue-worker, and HTTP-server threads. Those vectors
            // belong to mod-playerbots and are read from this thread with no
            // locking, so this is the only place they may be written.
            Hs_DrainExcludeVectorQueue();

            _msSinceReconcile += diff;
            if (_msSinceReconcile >= kIdentityReconcileIntervalMs)
            {
                _msSinceReconcile = 0;
                Hs_ApplyExcludeVectorsFromIdentityTable();
            }

            _msSinceSweep += diff;
            if (_msSinceSweep >= kIdentitySweepIntervalMs)
            {
                _msSinceSweep = 0;
                Hs_RunIdentityDailySweep();
                StampSweepRun(kSweepIdentityDaily);
            }
        }

    private:
        uint32 _msSinceReconcile = 0;
        uint32 _msSinceSweep     = 0;
    };

    // Corpus eviction: exposure-first over-quota trimming, then the
    // age-based unused-row sweep. Its own WorldScript rather than folded
    // into the identity one above (corpus and identity are unrelated
    // subsystems that happen to share a once-daily cadence). Runs
    // unconditionally (not gated on g_HsGeneratorEnabled), since a bucket
    // can go over quota via `.hearthside capture` or a lowered
    // RowsPerBucket even while generation itself is off.
    constexpr uint32_t kCorpusEvictionIntervalMs = 86400000;

    class HsCorpusLifecycleWorldScript : public WorldScript
    {
    public:
        HsCorpusLifecycleWorldScript() : WorldScript("HsCorpusLifecycleWorldScript") {}
        void OnStartup() override
        {
            // Review B4, as HsIdentityLifecycleWorldScript above.
            _msSinceEviction = SweepBacklogMs(kSweepCorpusEviction, kCorpusEvictionIntervalMs);
        }
        void OnUpdate(uint32 diff) override
        {
            _msSinceEviction += diff;
            if (_msSinceEviction < kCorpusEvictionIntervalMs)
                return;
            _msSinceEviction = 0;
            Hs_RunEvictionSweep();
            Hs_RunUnusedRowEvictionSweep();
            StampSweepRun(kSweepCorpusEviction);
        }

    private:
        uint32 _msSinceEviction = 0;
    };

    // The authenticated HTTP control API's lifecycle. Registered after
    // HsConfigWorldScript so g_HsHttpServer* is loaded before Start() reads
    // it. A bind failure or missing private key is logged and leaves the
    // server off (hs_http_server.cpp); this WorldScript doesn't need to
    // know which happened.
    class HsHttpServerWorldScript : public WorldScript
    {
    public:
        HsHttpServerWorldScript() : WorldScript("HsHttpServerWorldScript") {}
        void OnStartup() override { Hs_HttpServerStart(); }
        void OnShutdown() override { Hs_HttpServerStop(); }
    };

    // Rolling metrics sampler, on the same tick-driven shape as the other
    // periodic sweeps in this file. Its own accumulator since none of
    // them share its cadence.
    class HsMetricsWorldScript : public WorldScript
    {
    public:
        HsMetricsWorldScript() : WorldScript("HsMetricsWorldScript") {}
        void OnUpdate(uint32 diff) override
        {
            _msSinceSample += diff;
            if (_msSinceSample < kHsMetricsSampleIntervalSeconds * 1000)
                return;
            _msSinceSample = 0;
            Hs_SampleMetrics();
        }

    private:
        uint32 _msSinceSample = 0;
    };
}

void Addmod_hearthside_chatScripts()
{
    LOG_INFO("module.hearthside", "[HearthsideChat] Registering mod-hearthside-chat scripts.");
    new HsConfigWorldScript();
    new HsArchetypeLifecycleWorldScript();
    new HsGroundedLifecycleWorldScript();
    new HsEventAffinityLifecycleWorldScript();
    new HsChatHandler();
    new HsBridgePlayerScript();
    new HsDeliveryWorldScript();
    new HsCommandScript();
    new HsOpenerGroupHandler();
    new HsOpenerKillHandler();
    new HsOpenerResurrectHandler();
    new HsOpenerEncounterHandler();
    new HsMemoryDeathHandler();
    new HsMemoryGuildHandler();
    // Event triggers (hs_event.h). HsEventDeathHandler takes the same
    // PLAYERHOOK_ON_PLAYER_JUST_DIED as HsMemoryDeathHandler above; both
    // run, and neither depends on the other's ordering.
    new HsEventDeathHandler();
    new HsEventLevelHandler();
    new HsEventPvpKillHandler();
    new HsEventRollHandler();
    new HsEventDuelHandler();
    new HsScriptRunnerWorldScript();
    new HsEngagementScanWorldScript();
    // Registered after the script runner deliberately: both scan on the same
    // 30s cadence and draw from the same speakers, and ambient asks the
    // script runner whether a bot is already mid-scene
    // (Hs_IsBotInAnyScriptRun). Neither depends on the other's registration
    // order for correctness (both are tick-driven, and the query reads a
    // mutex-guarded map), but keeping them adjacent keeps the pairing
    // visible to whoever reads this list next.
    new HsAmbientScanWorldScript();
    new HsQueueLifecycleWorldScript();
    new HsGeneratorLifecycleWorldScript();
    new HsIdentityLifecycleWorldScript();
    new HsCorpusLifecycleWorldScript();
    new HsHttpServerWorldScript();
    new HsMetricsWorldScript();
}
