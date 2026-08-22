#include "hs_archetype_store.h"
#include "hs_config.h"
#include "hs_engagement.h"
#include "hs_generator.h"
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
    // New 2026-08-21: loads the archetype table from hside_archetype (§4.11,
    // moved out of a compiled constant so weights/care/reply/cap/talksAbout/
    // profanity are retunable without a rebuild). Its own WorldScript, same
    // "kept separate from HsConfigWorldScript so config loading stays
    // config-only" reasoning the queue lifecycle script below already uses --
    // registered right after it and before anything that could draw an
    // archetype (the queue worker, reflex/grounded/corpus replies, all of
    // which run only once a player is in the world, well after startup).
    class HsArchetypeLifecycleWorldScript : public WorldScript
    {
    public:
        HsArchetypeLifecycleWorldScript() : WorldScript("HsArchetypeLifecycleWorldScript") {}
        void OnStartup() override { Hs_LoadArchetypesFromDb(); }
        void OnAfterConfigLoad(bool reload) override
        {
            // `.reload config` doesn't touch hside_archetype, but an operator
            // editing the table directly and wanting a live pick-up without a
            // full restart is exactly what `.reload config` already means for
            // every other HearthsideChat.* setting -- reusing that trigger
            // rather than inventing a new GM command for one table.
            if (reload)
                Hs_LoadArchetypesFromDb();
        }
    };

    // Starts/stops the §4.3 runtime queue's worker thread. Kept separate from
    // HsConfigWorldScript so config loading stays config-only; registered
    // after it so the worker never starts before HearthsideChat.* is loaded.
    class HsQueueLifecycleWorldScript : public WorldScript
    {
    public:
        HsQueueLifecycleWorldScript() : WorldScript("HsQueueLifecycleWorldScript") {}
        void OnStartup() override { Hs_QueueStartup(); }
        void OnShutdown() override { Hs_QueueShutdown(); }
    };

    // Same lifecycle shape for the §4.7 idle-time generator's own
    // background thread (step 12). Separate from the queue's -- the
    // generator only ever *calls into* Hs_IsReactiveIdle(), it doesn't
    // share the reactive worker thread.
    class HsGeneratorLifecycleWorldScript : public WorldScript
    {
    public:
        HsGeneratorLifecycleWorldScript() : WorldScript("HsGeneratorLifecycleWorldScript") {}
        void OnStartup() override { Hs_GeneratorStartup(); }
        void OnShutdown() override { Hs_GeneratorShutdown(); }
    };

    // §4.13's exclusion-vector projection (trap 16): `hside_identity` is the
    // source of truth, sPlayerbotAIConfig's two exclude vectors are a
    // projection of it, re-applied at startup, on every `.reload config`,
    // and periodically -- playerbots' own OnAfterConfigLoad handler
    // unconditionally clears both vectors from playerbots.conf on every
    // reload, which has no way to encode these names.
    //
    // §4.13's own decision framework named the test to run and the fix to
    // ship if it failed: "promote a bot, confirm its name is in
    // resetBotLevelExcludeNames, issue .reload config, and re-read the
    // vector. Present -> ordering is favourable and the timer is
    // unnecessary. Absent -> ... the periodic reconcile ships at 300s,
    // aligned with the bracket check." Run live against the test realm
    // (step 15): promoted+carded a real bot, confirmed via gdb that its
    // name landed in both vectors, issued `.reload config`, and re-checked
    // via gdb -- both vectors came back empty. Ordering is unfavorable on
    // this build, so the periodic reconcile below is not a defensive
    // extra; it's the specified outcome of a test that actually failed.
    constexpr uint32_t kIdentityReconcileIntervalMs = 300000;

    // §4.12 decay/pinning/retirement (§7 step 17). Shares this WorldScript
    // rather than getting its own -- the reconcile above already ticks on a
    // timer, and decay/dormancy operate on day/week/season-scale windows
    // (hs_identity.h's kHsScoreDecayGraceDays/kHsCardDormancyDays), so a
    // once-daily cadence is the natural unit rather than the reconcile's
    // 300s bracket-check alignment. See hs_identity_store.h's
    // Hs_RunIdentityDailySweep for what the sweep does.
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

    // §4.5/§4.6, §7 step 18: exposure-first corpus eviction. Its own
    // WorldScript rather than folded into the identity one above -- corpus
    // and identity are different subsystems that happen to both want a
    // once-daily cadence, not one lifecycle. Runs unconditionally (not
    // gated on g_HsGeneratorEnabled), since a bucket can go over quota via
    // `.hearthside capture` or a lowered RowsPerBucket even while
    // generation itself is off.
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
        }

    private:
        uint32 _msSinceEviction = 0;
    };

    // §4.19/§7 step 19: the authenticated HTTP control API's lifecycle.
    // Registered after HsConfigWorldScript so g_HsHttpServer* is loaded
    // before Start() reads it, same ordering reasoning as the queue/
    // generator lifecycle scripts above. A bind failure or missing private
    // key is logged and leaves the server off (hs_http_server.cpp) -- this
    // WorldScript doesn't need to know which happened.
    class HsHttpServerWorldScript : public WorldScript
    {
    public:
        HsHttpServerWorldScript() : WorldScript("HsHttpServerWorldScript") {}
        void OnStartup() override { Hs_HttpServerStart(); }
        void OnShutdown() override { Hs_HttpServerStop(); }
    };

    // §4.19/§7 step 19: the rolling metrics sampler. "~5 minutes" per §6 --
    // its own accumulator on the same tick-driven shape as every other
    // periodic sweep in this file, not folded into an existing WorldScript
    // since none of them share this cadence.
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
    new HsChatHandler();
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
