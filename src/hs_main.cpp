#include "hs_archetype_store.h"
#include "hs_bridge.h"
#include "hs_config.h"
#include "hs_engagement.h"
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
#include "Log.h"

namespace
{
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
            // loaded above, so this must come second. Startup-only -- a
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
    // and lifecycle as HsArchetypeLifecycleWorldScript above -- registered
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
    // Kept separate from the queue's -- the generator only calls into
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
    // getting its own -- decay/dormancy operate on day/week-scale windows
    // (hs_identity.h's kHsScoreDecayGraceDays/kHsCardDormancyDays), so a
    // once-daily cadence fits better than the reconcile's 300s interval.
    // See Hs_RunIdentityDailySweep (hs_identity_store.h) for what it does.
    constexpr uint32_t kIdentitySweepIntervalMs = 86400000;

    class HsIdentityLifecycleWorldScript : public WorldScript
    {
    public:
        HsIdentityLifecycleWorldScript() : WorldScript("HsIdentityLifecycleWorldScript") {}
        void OnStartup() override { Hs_ApplyExcludeVectorsFromIdentityTable(); }
        void OnAfterConfigLoad(bool reload) override
        {
            if (reload)
                Hs_ApplyExcludeVectorsFromIdentityTable();
        }
        void OnUpdate(uint32 diff) override
        {
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
            }
        }

    private:
        uint32 _msSinceReconcile = 0;
        uint32 _msSinceSweep     = 0;
    };

    // Corpus eviction: exposure-first over-quota trimming, then the
    // age-based unused-row sweep. Its own WorldScript rather than folded
    // into the identity one above -- corpus and identity are unrelated
    // subsystems that happen to share a once-daily cadence. Runs
    // unconditionally (not gated on g_HsGeneratorEnabled), since a bucket
    // can go over quota via `.hearthside capture` or a lowered
    // RowsPerBucket even while generation itself is off.
    constexpr uint32_t kCorpusEvictionIntervalMs = 86400000;

    class HsCorpusLifecycleWorldScript : public WorldScript
    {
    public:
        HsCorpusLifecycleWorldScript() : WorldScript("HsCorpusLifecycleWorldScript") {}
        void OnUpdate(uint32 diff) override
        {
            _msSinceEviction += diff;
            if (_msSinceEviction < kCorpusEvictionIntervalMs)
                return;
            _msSinceEviction = 0;
            Hs_RunEvictionSweep();
            Hs_RunUnusedRowEvictionSweep();
        }

    private:
        uint32 _msSinceEviction = 0;
    };

    // The authenticated HTTP control API's lifecycle. Registered after
    // HsConfigWorldScript so g_HsHttpServer* is loaded before Start() reads
    // it. A bind failure or missing private key is logged and leaves the
    // server off (hs_http_server.cpp) -- this WorldScript doesn't need to
    // know which happened.
    class HsHttpServerWorldScript : public WorldScript
    {
    public:
        HsHttpServerWorldScript() : WorldScript("HsHttpServerWorldScript") {}
        void OnStartup() override { Hs_HttpServerStart(); }
        void OnShutdown() override { Hs_HttpServerStop(); }
    };

    // Rolling metrics sampler, on the same tick-driven shape as the other
    // periodic sweeps in this file -- its own accumulator since none of
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
    LOG_INFO("server.loading", "[HearthsideChat] Registering mod-hearthside-chat scripts.");
    new HsConfigWorldScript();
    new HsArchetypeLifecycleWorldScript();
    new HsGroundedLifecycleWorldScript();
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
    new HsScriptRunnerWorldScript();
    new HsEngagementScanWorldScript();
    new HsQueueLifecycleWorldScript();
    new HsGeneratorLifecycleWorldScript();
    new HsIdentityLifecycleWorldScript();
    new HsCorpusLifecycleWorldScript();
    new HsHttpServerWorldScript();
    new HsMetricsWorldScript();
}
