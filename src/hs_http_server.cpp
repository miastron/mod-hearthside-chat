#include "hs_http_server.h"
#include "hs_botchain.h"
#include "hs_config.h"
#include "hs_event.h"
#include "hs_generator.h"
#include "hs_http_auth.h"
#include "hs_identity_store.h"
#include "hs_json.h"
#include "hs_memory_store.h"
#include "hs_opener.h"
#include "hs_queue.h"
#include "hs_script.h"
#include "hs_metrics.h"

// Same rename-on-include technique as hs_llm.cpp -- two other modules in
// this workspace already vendor different cpp-httplib versions under the
// unqualified `httplib` namespace, and this is a third translation unit
// pulling the header into the same worldserver binary.
#define httplib hs_httplib
#include <httplib.h>
#undef httplib

#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "Log.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace
{
    std::unique_ptr<hs_httplib::Server> s_server;
    std::unique_ptr<std::thread>        s_thread;
    std::atomic<bool>                   s_running{false};

    uint8_t LookupBotLevel(uint64_t botGuid)
    {
        QueryResult result = CharacterDatabase.Query("SELECT level FROM characters WHERE guid = {}", botGuid);
        return result ? (*result)[0].Get<uint8_t>() : 0;
    }

    uint64_t ParseGuidParam(const hs_httplib::Request& req)
    {
        auto it = req.path_params.find("guid");
        if (it == req.path_params.end())
            return 0;
        try { return std::stoull(it->second); }
        catch (...) { return 0; }
    }

    void SendJson(hs_httplib::Response& res, const hs_json& body, int status = 200)
    {
        res.status = status;
        res.set_content(body.dump(), "application/json");
    }

    void SendError(hs_httplib::Response& res, int status, const std::string& message)
    {
        SendJson(res, hs_json{ {"error", message} }, status);
    }

    // Read-half gate: a valid bearer token only. Returns false (and has
    // already written the 401 response) if authentication fails.
    bool RequireAuth(const hs_httplib::Request& req, hs_httplib::Response& res)
    {
        std::string token = Hs_ExtractBearerToken(req.get_header_value("Authorization"));
        if (token.empty())
        {
            SendError(res, 401, "Missing Authorization header. Use Authorization: Bearer <token>");
            return false;
        }
        if (!Hs_ValidateBearerToken(token))
        {
            SendError(res, 401, "Invalid token");
            return false;
        }
        return true;
    }

    // Mutating-half gate: valid token and HttpControlEnable. Checked after
    // RequireAuth at each control route's own call site rather than folded
    // into one combined helper, so a disabled-control 403 is distinguishable
    // from an unauthenticated 401.
    bool RequireControlEnabled(hs_httplib::Response& res)
    {
        if (!g_HsHttpControlEnable)
        {
            SendError(res, 403, "Control API disabled (HearthsideChat.HttpControlEnable = 0)");
            return false;
        }
        return true;
    }

    hs_json StatusJson()
    {
        hs_json j;
        j["enable"]         = g_HsEnable;
        j["backend_down"]   = Hs_IsBackendDown();
        j["queue_depth"]    = Hs_PendingQueueDepth();
        j["queue_max_depth"] = g_HsQueueMaxDepth;
        // Hs_ConfigString, not a direct read: this route runs on the HTTP
        // server thread, and `.reload config` reassigns these from the world
        // thread. The equivalent read in `.hearthside status`
        // (hs_command.cpp) is safe unguarded because the GM command handler
        // *is* the world thread. See hs_config.h's cross-thread section.
        j["max_tier"] = {
            {"direct_reply", Hs_ConfigString(g_HsMaxTierDirectReply)},
            {"ambient",      Hs_ConfigString(g_HsMaxTierAmbient)},
            {"openers",      Hs_ConfigString(g_HsMaxTierOpeners)},
            {"bot_to_bot",   Hs_ConfigString(g_HsMaxTierBotToBot)},
            {"reflex",       Hs_ConfigString(g_HsMaxTierReflex)},
            {"events",       Hs_ConfigString(g_HsMaxTierEvents)},
        };
        j["generator"] = {
            {"enabled",               g_HsGeneratorEnabled},
            {"rows_added_session",    Hs_GeneratorRowsAddedThisSession()},
            {"rows_evicted_session", Hs_RowsEvictedThisSession()},
        };
        j["openers_fired_session"]         = Hs_OpenersFiredThisSession();
        j["events_fired_session"]          = Hs_EventsFiredThisSession();
        j["botchain_hops_fired_session"]   = Hs_BotChainHopsFiredThisSession();
        j["script"] = {
            {"reserve_depth",     Hs_ScriptReserveDepth()},
            {"active_runs",       Hs_ActiveScriptRunCount()},
            {"consumed_last_24h", Hs_ScriptsConsumedLast24h()},
        };
        j["identity"] = {
            {"row_count",           Hs_IdentityRowCount()},
            {"card_active_count",   Hs_CardActiveCount()},
            {"promotions_session", Hs_PromotionsThisSession()},
            {"demotions_session",  Hs_DemotionsThisSession()},
            {"retirements_session", Hs_RetirementsThisSession()},
        };
        j["memory_row_count"] = Hs_MemoryRowCount();
        return j;
    }

    hs_json InspectionJson(uint64_t botGuid, const HsIdentityInspection& insp)
    {
        hs_json j;
        j["bot_guid"]          = botGuid;
        j["has_identity_row"] = insp.hasIdentityRow;
        if (!insp.hasIdentityRow)
            return j;
        j["archetype"]         = insp.archetype;
        j["last_known_level"] = insp.lastKnownLevel;
        j["interaction_score"] = insp.interactionScore;
        j["promoted"]           = insp.promoted;
        j["card_active"]       = insp.cardActive;
        j["pinned_by_friend"]  = insp.pinnedByFriend;
        j["has_any_memory_rows"] = insp.hasAnyMemoryRows;
        if (insp.cardActive)
            j["voice_block"] = insp.voiceBlock;
        return j;
    }

    void RegisterRoutes(hs_httplib::Server& svr)
    {
        svr.Get("/", [](const hs_httplib::Request&, hs_httplib::Response& res) {
            SendJson(res, hs_json{ {"module", "mod-hearthside-chat"}, {"ok", true} });
        });

        // ---- Read half: token only ----

        svr.Get("/api/status", [](const hs_httplib::Request& req, hs_httplib::Response& res) {
            if (!RequireAuth(req, res)) return;
            SendJson(res, StatusJson());
        });

        svr.Get("/api/metrics", [](const hs_httplib::Request& req, hs_httplib::Response& res) {
            if (!RequireAuth(req, res)) return;
            uint32_t limit = 50;
            if (req.has_param("limit"))
            {
                try { limit = std::min<uint32_t>(500, std::stoul(req.get_param_value("limit"))); }
                catch (...) { /* keep default */ }
            }
            hs_json arr = hs_json::array();
            for (auto const& s : Hs_RecentMetrics(limit))
            {
                arr.push_back({
                    {"sampled_at",                  s.sampledAt},
                    {"backend_down",                s.backendDown},
                    {"queue_depth",                 s.queueDepth},
                    {"corpus_row_count",            s.corpusRowCount},
                    {"corpus_rows_added_session",   s.corpusRowsAddedSession},
                    {"corpus_rows_evicted_session", s.corpusRowsEvictedSession},
                    {"script_reserve_depth",        s.scriptReserveDepth},
                    {"script_active_runs",          s.scriptActiveRuns},
                    {"script_consumed_24h",         s.scriptConsumed24h},
                    {"identity_row_count",          s.identityRowCount},
                    {"card_active_count",           s.cardActiveCount},
                    {"promotions_session",          s.promotionsSession},
                    {"demotions_session",           s.demotionsSession},
                    {"retirements_session",         s.retirementsSession},
                    {"memory_row_count",            s.memoryRowCount},
                    {"openers_fired_session",       s.openersFiredSession},
                    {"latency_p50_ms",               s.latencyP50Ms},
                    {"latency_p95_ms",               s.latencyP95Ms},
                    {"latency_p99_ms",               s.latencyP99Ms},
                    {"prompt_chars_ring1_mean",      s.promptCharsRing1Mean},
                    {"prompt_chars_ring2_mean",      s.promptCharsRing2Mean},
                    {"prompt_chars_ring3_mean",      s.promptCharsRing3Mean},
                    {"ttl_dropped_session",          s.ttlDroppedSession},
                    {"ttl_processed_session",        s.ttlProcessedSession},
                    {"bucket_denied_session",        s.bucketDeniedSession},
                    {"bucket_attempted_session",     s.bucketAttemptedSession},
                });
            }
            SendJson(res, arr);
        });

        // Per-archetype/per-channel reply-vs-silence counts -- its own route
        // rather than folded into /api/metrics, since it's a different row
        // shape (one row per dimension/key per interval, not one row per
        // interval) -- see hs_metrics.h's HsMetricsBreakdownRow doc comment.
        svr.Get("/api/metrics/breakdown", [](const hs_httplib::Request& req, hs_httplib::Response& res) {
            if (!RequireAuth(req, res)) return;
            uint32_t limit = 200;
            if (req.has_param("limit"))
            {
                try { limit = std::min<uint32_t>(2000, std::stoul(req.get_param_value("limit"))); }
                catch (...) { /* keep default */ }
            }
            hs_json arr = hs_json::array();
            for (auto const& row : Hs_RecentMetricsBreakdown(limit))
            {
                arr.push_back({
                    {"sampled_at",    row.sampledAt},
                    {"dimension",     row.dimension},
                    {"key",           row.key},
                    {"replied_count", row.repliedCount},
                    {"silent_count",  row.silentCount},
                });
            }
            SendJson(res, arr);
        });

        svr.Get("/api/bot/:guid", [](const hs_httplib::Request& req, hs_httplib::Response& res) {
            if (!RequireAuth(req, res)) return;
            uint64_t botGuid = ParseGuidParam(req);
            if (botGuid == 0) { SendError(res, 400, "Invalid guid"); return; }
            SendJson(res, InspectionJson(botGuid, Hs_InspectIdentity(botGuid)));
        });

        svr.Get("/api/corpus/:category", [](const hs_httplib::Request& req, hs_httplib::Response& res) {
            if (!RequireAuth(req, res)) return;
            std::string category      = req.path_params.at("category");
            std::string promptVersion = req.has_param("prompt_version") ? req.get_param_value("prompt_version") : "";
            hs_json arr = hs_json::array();
            for (auto const& row : Hs_ReviewCorpusRows(category, promptVersion, 50))
            {
                arr.push_back({
                    {"text",           row.text},
                    {"times_used",     row.timesUsed},
                    {"model",          row.model},
                    {"prompt_version", row.promptVersion},
                });
            }
            SendJson(res, arr);
        });

        // ---- Control half: token AND HttpControlEnable ----

        svr.Post("/api/bot/:guid/promote", [](const hs_httplib::Request& req, hs_httplib::Response& res) {
            if (!RequireAuth(req, res)) return;
            if (!RequireControlEnabled(res)) return;
            uint64_t botGuid = ParseGuidParam(req);
            uint8_t  level   = LookupBotLevel(botGuid);
            if (botGuid == 0 || level == 0) { SendError(res, 404, "Bot not found"); return; }
            bool promoted = Hs_ForcePromote(botGuid, level);
            SendJson(res, hs_json{ {"bot_guid", botGuid}, {"promoted", promoted} });
        });

        svr.Post("/api/bot/:guid/demote", [](const hs_httplib::Request& req, hs_httplib::Response& res) {
            if (!RequireAuth(req, res)) return;
            if (!RequireControlEnabled(res)) return;
            uint64_t botGuid = ParseGuidParam(req);
            if (botGuid == 0) { SendError(res, 400, "Invalid guid"); return; }
            bool demoted = Hs_ForceDemote(botGuid);
            SendJson(res, hs_json{ {"bot_guid", botGuid}, {"demoted", demoted} });
        });

        svr.Post("/api/bot/:guid/retire", [](const hs_httplib::Request& req, hs_httplib::Response& res) {
            if (!RequireAuth(req, res)) return;
            if (!RequireControlEnabled(res)) return;
            uint64_t botGuid = ParseGuidParam(req);
            uint8_t  level   = LookupBotLevel(botGuid);
            if (botGuid == 0 || level == 0) { SendError(res, 404, "Bot not found"); return; }
            Hs_RetireCard(botGuid, level);
            SendJson(res, hs_json{ {"bot_guid", botGuid}, {"retired", true} });
        });

        svr.Post("/api/bot/:guid/pin", [](const hs_httplib::Request& req, hs_httplib::Response& res) {
            if (!RequireAuth(req, res)) return;
            if (!RequireControlEnabled(res)) return;
            uint64_t botGuid = ParseGuidParam(req);
            if (botGuid == 0) { SendError(res, 400, "Invalid guid"); return; }
            Hs_GmPinBot(botGuid);
            SendJson(res, hs_json{ {"bot_guid", botGuid}, {"pinned", true} });
        });

        svr.Post("/api/bot/:guid/unpin", [](const hs_httplib::Request& req, hs_httplib::Response& res) {
            if (!RequireAuth(req, res)) return;
            if (!RequireControlEnabled(res)) return;
            uint64_t botGuid = ParseGuidParam(req);
            if (botGuid == 0) { SendError(res, 400, "Invalid guid"); return; }
            Hs_GmUnpinBot(botGuid);
            SendJson(res, hs_json{ {"bot_guid", botGuid}, {"pinned", false} });
        });

        svr.Delete("/api/corpus/run/:prompt_version", [](const hs_httplib::Request& req, hs_httplib::Response& res) {
            if (!RequireAuth(req, res)) return;
            if (!RequireControlEnabled(res)) return;
            std::string promptVersion = req.path_params.at("prompt_version");
            uint32_t evicted = Hs_EvictGenerationRun(promptVersion);
            SendJson(res, hs_json{ {"prompt_version", promptVersion}, {"rows_evicted", evicted} });
        });
    }
}

void Hs_HttpServerStart()
{
    if (g_HsHttpServerPort == 0)
        return;

    if (g_HsHttpServerPrivateKey.empty())
    {
        LOG_ERROR("server.loading",
            "[HearthsideChat] HTTP server not started: HearthsideChat.HttpServerPrivateKey is not set.");
        return;
    }

    try
    {
        auto svr = std::make_unique<hs_httplib::Server>();
        RegisterRoutes(*svr);
        svr->set_read_timeout(static_cast<int>(g_HsHttpServerTimeoutSeconds));
        svr->set_write_timeout(static_cast<int>(g_HsHttpServerTimeoutSeconds));

        if (!svr->bind_to_port(g_HsHttpServerBind.c_str(), static_cast<int>(g_HsHttpServerPort)))
        {
            LOG_ERROR("server.loading",
                "[HearthsideChat] HTTP server failed to bind to {}:{} -- port may be in use or address invalid. "
                "HTTP server disabled; the rest of the module continues normally.",
                g_HsHttpServerBind, g_HsHttpServerPort);
            return;
        }

        s_server = std::move(svr);
        s_running.store(true);

        // Bind address captured by value rather than read from the global
        // inside the thread: g_HsHttpServerBind is a namespace-scope
        // std::string in another translation unit, so reading it from this
        // thread races both `.reload config` and static destruction at
        // process exit.
        std::string bindAddr = g_HsHttpServerBind;
        uint32_t    bindPort = g_HsHttpServerPort;
        s_thread = std::make_unique<std::thread>([bindAddr, bindPort]() {
            LOG_INFO("server.loading", "[HearthsideChat] HTTP server listening on {}:{}", bindAddr, bindPort);
            s_server->listen_after_bind();
            s_running.store(false);
            LOG_INFO("server.loading", "[HearthsideChat] HTTP server stopped.");
        });
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("server.loading",
            "[HearthsideChat] HTTP server exception during startup: {}. HTTP server disabled; "
            "the rest of the module continues normally.", ex.what());
        s_server.reset();
        s_running.store(false);
    }
}

void Hs_HttpServerStop()
{
    if (s_server)
    {
        // wait_until_ready() before stop(), and it is load-bearing rather
        // than tidiness. bind_to_port() runs on the world thread in
        // Hs_HttpServerStart, but Server::is_running_ is only set once the
        // listener thread is actually inside listen_internal(); stop() is a
        // no-op while that flag is false (httplib.h:10973). A startup
        // immediately followed by a shutdown can land in exactly that
        // window, after which the loop would start against a still-valid
        // socket and never exit -- turning the join below into a hang.
        // wait_until_ready() closes the window (httplib.h:10967).
        s_server->wait_until_ready();
        s_server->stop();
    }

    // Join, not detach. The lambda dereferences the global s_server and
    // reads g_HsHttpServerBind (a namespace-scope std::string in another
    // translation unit); detaching does not stop static destruction from
    // destroying either one out from under a thread still unwinding out of
    // listen_after_bind() or executing its trailing LOG_INFO.
    //
    // The join is bounded: stop() closes the listen socket, so the accept
    // loop breaks on its next iteration (httplib.h:11590-11597), and the
    // task queue's own shutdown then waits only for in-flight requests,
    // themselves bounded by HttpServerTimeoutSeconds.
    if (s_thread && s_thread->joinable())
        s_thread->join();
    s_thread.reset();
    // Safe to release the server now, unlike under the old detach: no
    // thread can still be inside it once the join has returned.
    s_server.reset();
    s_running.store(false);
}

bool Hs_HttpServerIsRunning()
{
    return s_running.load();
}
