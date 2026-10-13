#include "hs_cloud.h"
#include "hs_cloud_fact.h"
#include "hs_config.h"
#include "hs_json.h"
#include "hs_llm.h"
#include "hs_log.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "QueryResult.h"

#define httplib hs_httplib
#include <httplib.h>
#undef httplib

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace
{
    // The instruction. Mirrored in Tests/haiku_fact_latency.py, which measured
    // it: keep the two in step. Haiku was the judge of "is this about the game"
    // over 112 WoW lines and 25 off-topic questions (2026-10-13) and answered
    // unknown to every WoW line; the router in front of it only saves calls.
    constexpr char kSystemBase[] =
        "You supply one real-world fact for a chat bot inside World of Warcraft to mention in passing. "
        "Reply with exactly one line: Title -- One plain sentence with the answer. "
        "Write dates as dates, never as \"yesterday\" or \"last year\". "
        "If the message is about World of Warcraft or any video game, is chat or a request rather than "
        "a question of fact, or you do not know the answer, reply with only the word unknown.";
    constexpr char kSystemNoSearch[] =
        " Also reply unknown when the answer depends on events after your training data.";
    constexpr char kSystemSearch[] =
        " If the answer may have changed since your training data, search before answering, and end the "
        "line with [days:N], the number of days the answer will stay true.";

    constexpr char kHost[]          = "api.anthropic.com";
    constexpr char kPath[]          = "/v1/messages";
    constexpr char kApiVersion[]    = "2023-06-01";
    constexpr char kSearchTool[]    = "web_search_20250305"; // 20260209 never searched on Haiku in testing
    constexpr size_t kMaxPending    = 16;
    constexpr uint64_t kRelativeDayTtlSeconds = 6 * 3600;

    struct HsCloudJob
    {
        HsReplyRequest                       req;
        std::function<void(HsReplyRequest)> resume;
    };

    std::mutex                 g_CloudMutex;
    std::condition_variable    g_CloudCv;
    std::deque<HsCloudJob>     g_CloudQueue;
    bool                       g_CloudStop = true;
    std::thread                g_CloudThread;

    // Cloud thread only after startup (the load runs before the thread starts).
    HsCloudFactStore    g_Store;
    HsCloudDailyLimiter g_Limiter;

    uint64_t UnixNow()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    // Server-local calendar day: the daily limit resets at local midnight.
    uint32_t LocalDay()
    {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        return static_cast<uint32_t>(tm.tm_year) * 1000u + static_cast<uint32_t>(tm.tm_yday);
    }

    // The fail-safe's third layer. Anything that is not a plain reply to a
    // real player's own line is refused here, whatever let it through.
    bool StructurallyEligible(HsReplyRequest const& req)
    {
        return req.cloudAccountId != 0 && !req.isEvent && !req.isFollowUp && !req.triggerIsStateLine &&
               req.chainScopeId == 0 && req.senderGuid != 0 && req.senderGuid != req.botGuid;
    }

    bool FeatureOn(HsCloudStrings const& s)
    {
        return g_HsCloudEnable && !s.apiKey.empty() && !s.model.empty();
    }

    struct CallOutcome
    {
        bool        ok = false;       // HTTP 200 with a parseable body
        std::string text;             // the answer text, every block after the last search result joined
        uint32_t    searches = 0;
        uint32_t    latencyMs = 0;
        int         httpStatus = 0;
    };

    CallOutcome CallCloud(HsCloudStrings const& s, std::string const& question)
    {
        CallOutcome out;

        hs_json body;
        body["model"]      = s.model;
        body["max_tokens"] = 300;
        body["system"]     = std::string(kSystemBase) + (g_HsCloudWebSearchEnable ? kSystemSearch : kSystemNoSearch);
        // Measured best for a one-line lookup: medium effort with thinking off
        // (902 ms median; adaptive thinking added slow outliers).
        body["thinking"]      = { { "type", "disabled" } };
        body["output_config"] = { { "effort", s.effort.empty() ? "medium" : s.effort } };
        body["messages"]      = hs_json::array({ { { "role", "user" }, { "content", question } } });
        if (g_HsCloudWebSearchEnable && !s.webSearchDomain.empty())
            body["tools"] = hs_json::array({ { { "type", kSearchTool }, { "name", "web_search" }, { "max_uses", 1 },
                                               { "allowed_domains", hs_json::array({ s.webSearchDomain }) } } });

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
        LOG_ERROR(kHsLogLlm, "[HearthsideChat] Cloud facts need HTTPS, and this build has no OpenSSL.");
        return out;
#else
        static thread_local std::unique_ptr<hs_httplib::SSLClient> s_client;
        if (!s_client)
        {
            s_client = std::make_unique<hs_httplib::SSLClient>(kHost, 443);
            // Unlike the local LLM client: this one carries an API key over the
            // internet, so the server's certificate is checked against the
            // system store.
            s_client->enable_server_certificate_verification(true);
            s_client->set_keep_alive(true);
        }
        int timeout = static_cast<int>(g_HsCloudTimeoutSeconds ? g_HsCloudTimeoutSeconds : 10);
        s_client->set_connection_timeout(timeout);
        s_client->set_read_timeout(timeout);
        s_client->set_write_timeout(timeout);

        hs_httplib::Headers headers = {
            { "x-api-key", s.apiKey },
            { "anthropic-version", kApiVersion },
            { "User-Agent", "mod-hearthside-chat/1.0" },
        };

        auto start = std::chrono::steady_clock::now();
        hs_httplib::Result res = s_client->Post(kPath, headers, body.dump(), "application/json");
        out.latencyMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());

        if (!res)
        {
            LOG_ERROR(kHsLogLlm, "[HearthsideChat] Cloud request failed: {} after {} ms.",
                hs_httplib::to_string(res.error()), out.latencyMs);
            s_client.reset(); // a fresh connection next time, in case this one is wedged
            return out;
        }
        out.httpStatus = res->status;
        if (res->status != 200)
        {
            // The body names the problem (bad key, unknown model, no credit);
            // never the key itself, which only travels in a header.
            LOG_ERROR(kHsLogLlm, "[HearthsideChat] Cloud HTTP {}: {}", res->status, res->body.substr(0, 300));
            return out;
        }

        hs_json doc = hs_json::parse(res->body, nullptr, false);
        if (doc.is_discarded() || !doc.contains("content") || !doc["content"].is_array())
        {
            LOG_ERROR(kHsLogLlm, "[HearthsideChat] Cloud reply was not the expected JSON.");
            return out;
        }

        // With search on, text before the search is preamble ("Let me check")
        // and the answer after it can arrive split across blocks by citations.
        std::string text;
        for (auto const& block : doc["content"])
        {
            std::string type = block.value("type", std::string());
            if (type == "web_search_tool_result")
                text.clear();
            else if (type == "text")
                text += block.value("text", std::string());
        }
        if (doc.contains("usage") && doc["usage"].contains("server_tool_use"))
            out.searches = doc["usage"]["server_tool_use"].value("web_search_requests", 0u);

        out.ok   = true;
        out.text = text;
        return out;
#endif
    }

    void SaveFact(HsCloudFactEntry const& e)
    {
        std::string q = e.question, f = e.fact;
        CharacterDatabase.EscapeString(q);
        CharacterDatabase.EscapeString(f);
        // Fire-and-forget: nothing reads this row back in this process; the
        // in-memory store already holds it.
        CharacterDatabase.Execute(
            "INSERT INTO hside_cloud_fact (question, fact, searched, created_at, expires_at) VALUES ('{}', '{}', {}, {}, {})",
            q, f, e.searched ? 1 : 0, UnixNow(), e.expiresAt);
    }

    // Fills req.cloudFact when it can. Never throws past here: this runs on
    // the cloud thread, where an escaping exception is std::terminate.
    void Process(HsReplyRequest& req)
    {
        req.cloudDone = true;

        if (!StructurallyEligible(req))
        {
            LOG_ERROR(kHsLogLlm, "[HearthsideChat] Cloud refused a request that is not a real player's reply "
                "(bot {}, sender {}, account {}). A caller set cloudAccountId where it must not.",
                req.botGuid, req.senderGuid, req.cloudAccountId);
            return;
        }

        HsCloudStrings const s = Hs_CloudStringsSnapshot();
        if (!FeatureOn(s))
            return;

        uint64_t const now = UnixNow();
        if (HsCloudFactEntry const* hit = g_Store.Find(req.prompt, now))
        {
            req.cloudFact = hit->fact;
            LOG_INFO(kHsLogChat, "[HearthsideChat] Cloud fact (saved) for \"{}\": {}", req.prompt, req.cloudFact);
            return;
        }

        uint32_t const day = LocalDay();
        if (g_Limiter.Used(req.cloudAccountId, day) >= g_HsCloudDailyLimitPerAccount)
        {
            if (g_HsDebugEnabled)
                LOG_INFO(kHsLogChat, "[HearthsideChat] Cloud daily limit reached for account {}.", req.cloudAccountId);
            return;
        }

        CallOutcome call = CallCloud(s, req.prompt);
        if (!call.ok)
            return;

        HsCloudReply reply = Hs_ParseCloudReply(call.text);
        // The limit counts what costs money: a fact, or a search. An
        // "unknown" costs about $0.0001 and is already capped by the reply
        // bucket and the per-bot cooldown, so it does not eat into a player's day.
        if (reply.known || call.searches > 0)
            g_Limiter.TryTake(req.cloudAccountId, day, g_HsCloudDailyLimitPerAccount);

        LOG_INFO(kHsLogChat, "[HearthsideChat] Cloud {} ms, {} search(es) for \"{}\": {}", call.latencyMs,
            call.searches, req.prompt, reply.known ? reply.fact : std::string("unknown"));
        if (!reply.known)
            return;

        uint64_t ttl;
        if (call.searches > 0)
        {
            uint32_t days = reply.days ? reply.days : 1;
            if (days > g_HsCloudWebSearchMaxCacheDays)
                days = g_HsCloudWebSearchMaxCacheDays;
            ttl = static_cast<uint64_t>(days) * 86400;
        }
        else
            ttl = static_cast<uint64_t>(g_HsCloudCacheDays) * 86400;
        if (Hs_CloudMentionsRelativeDay(req.prompt) && ttl > kRelativeDayTtlSeconds)
            ttl = kRelativeDayTtlSeconds;

        req.cloudFact = reply.fact;
        if (ttl > 0)
        {
            HsCloudFactEntry entry{ req.prompt, reply.fact, now + ttl, call.searches > 0 };
            g_Store.Put(entry);
            SaveFact(entry);
        }
    }

    void CloudLoop()
    {
        for (;;)
        {
            HsCloudJob job;
            {
                std::unique_lock<std::mutex> lock(g_CloudMutex);
                g_CloudCv.wait(lock, [] { return g_CloudStop || !g_CloudQueue.empty(); });
                if (g_CloudStop)
                    return;
                job = std::move(g_CloudQueue.front());
                g_CloudQueue.pop_front();
            }
            try
            {
                Process(job.req);
            }
            catch (std::exception const& e)
            {
                LOG_ERROR(kHsLogLlm, "[HearthsideChat] Cloud fact failed: {}", e.what());
                job.req.cloudFact.clear();
            }
            job.resume(std::move(job.req));
        }
    }

    // Router = model: the tuned model's ROUTE mode labels the line game,
    // world or chat. Only "world" goes out. Anything else, an unexpected
    // label or a failed call included, keeps the line local.
    bool ModelSaysWorld(std::string const& line)
    {
        HsLLMStrings const llm = Hs_LLMStringsSnapshot();
        HsLLMConfig cfg;
        cfg.apiType      = llm.apiType;
        cfg.baseUrl      = llm.url;
        cfg.model        = llm.model;
        cfg.apiKey       = llm.apiKey;
        cfg.timeoutSec   = static_cast<int>(g_HsLLMTimeoutSeconds);
        cfg.templateKind = llm.templateKind;
        cfg.maxTokens    = 3;
        cfg.dryMultiplier = 0.0f;
        cfg.temperature  = 0.0f;
        HsLLMResult r = Hs_CallLLM(cfg, llm.systemPrompt, "Mode: ROUTE", {}, line);
        if (g_HsDebugEnabled)
            LOG_INFO(kHsLogChat, "[HearthsideChat] Cloud route \"{}\" -> {} ({} ms)", line,
                r.success ? r.text : std::string("call failed"), r.latencyMs);
        if (!r.success)
            return false;
        std::string label;
        for (char c : r.text)
            if (std::isalpha(static_cast<unsigned char>(c)))
                label += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return label == "world";
    }
}

void Hs_CloudStartup()
{
    uint64_t const now = UnixNow();
    CharacterDatabase.DirectExecute("DELETE FROM hside_cloud_fact WHERE expires_at <= {}", now);
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT question, fact, searched, expires_at FROM hside_cloud_fact WHERE expires_at > {} ORDER BY id", now))
    {
        do
        {
            Field* f = result->Fetch();
            g_Store.Put({ f[0].Get<std::string>(), f[1].Get<std::string>(), f[3].Get<uint64_t>(), f[2].Get<uint8_t>() != 0 });
        } while (result->NextRow());
    }
    LOG_INFO(kHsLog, "[HearthsideChat] Cloud facts: {} saved answer(s) loaded.", g_Store.Size());

    std::lock_guard<std::mutex> lock(g_CloudMutex);
    g_CloudStop = false;
    if (!g_CloudThread.joinable())
        g_CloudThread = std::thread(CloudLoop);
}

void Hs_CloudShutdown()
{
    std::deque<HsCloudJob> abandoned;
    {
        std::lock_guard<std::mutex> lock(g_CloudMutex);
        g_CloudStop = true;
        abandoned.swap(g_CloudQueue);
    }
    g_CloudCv.notify_all();
    if (g_CloudThread.joinable())
        g_CloudThread.join();
    // Their resume pushes into a queue that is shutting down too; dropped.
}

bool Hs_CloudWantsRequest(HsReplyRequest const& req)
{
    if (req.cloudDone || !StructurallyEligible(req))
        return false;
    HsCloudStrings const s = Hs_CloudStringsSnapshot();
    if (!FeatureOn(s) || !Hs_CloudIsFactQuestion(req.prompt))
        return false;
    if (s.router == "model")
        return ModelSaysWorld(req.prompt);
    return true; // local: the cloud model itself answers unknown to game and chat lines
}

void Hs_CloudSubmit(HsReplyRequest req, std::function<void(HsReplyRequest)> resume)
{
    {
        std::lock_guard<std::mutex> lock(g_CloudMutex);
        if (!g_CloudStop && g_CloudQueue.size() < kMaxPending)
        {
            g_CloudQueue.push_back({ std::move(req), std::move(resume) });
            g_CloudCv.notify_one();
            return;
        }
    }
    // Full or stopped: answered locally, as if the feature were off.
    req.cloudDone = true;
    resume(std::move(req));
}
