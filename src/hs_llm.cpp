#include "hs_llm.h"
#include "hs_json.h"
#include "Log.h"

// Rename httplib's namespace to avoid an ODR violation: mod-ollama-chat and
// mod-game-state-api each already vendor a different version of cpp-httplib
// under the unqualified `httplib` namespace, and all three land in the same
// worldserver binary. mod-playerbots-characters solved this the same way;
// this module follows suit under its own name.
#define httplib hs_httplib
#include <httplib.h>
#undef httplib

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>

using json = hs_json;

namespace
{
    bool IEquals(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }

    std::string Trim(std::string s)
    {
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        return s;
    }

    // mod-ollama-chat's ExtractTextBetweenDoubleQuotes truncates at the
    // first two quote characters anywhere in the reply, silently mangling
    // any line that quotes a word. Only strip a leading/trailing quote pair
    // that wraps the *entire* string.
    std::string StripWrappingQuotes(std::string s)
    {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            s = s.substr(1, s.size() - 2);
        return s;
    }

    // Chat lines are single-line by construction. Style/format is applied
    // at delivery, never baked into stored or cached text; this just makes
    // the raw model output fit one chat line.
    std::string CollapseNewlines(std::string s)
    {
        for (char& c : s)
            if (c == '\n' || c == '\r')
                c = ' ';
        return s;
    }

    // Fixed, byte-identical for every bot. Teaches register (casual/
    // short/lowercase) rather than subject matter, which would leak answers
    // into unrelated replies. Deliberately off-topic from anything a bot
    // will actually be asked. Widened from the original 5 (all short
    // affirmative/compliant replies) to also cover the registers real chat
    // needs and the model had never been shown: a non-answer, a question
    // thrown back, a flat one-word brush-off, and a subject change.
    const std::vector<std::pair<std::string, std::string>>& Fewshot()
    {
        static const std::vector<std::pair<std::string, std::string>> examples =
        {
            { "you around later?", "prob yeah, after work" },
            { "did you see what they did to the patch notes", "lol yeah" },
            { "hey can i ask you something", "sure" },
            { "what do you think", "eh. not sure tbh" },
            { "thanks!!", "np" },
            // non-answer
            { "so what's the deal with that", "honestly couldn't tell you" },
            // question thrown back
            { "you doing anything fun this weekend", "eh not really, you?" },
            // one-word reply
            { "you good?", "yeah" },
            // subject change
            { "man that fight was rough", "yeah. anyway you selling that or keeping it" },
        };
        return examples;
    }

    // ------------------------------------------------------------------
    // Chat-markup dialects for apiType=llamacpp's native /completion.
    //
    // That endpoint bypasses llama.cpp's own template handling entirely (it
    // takes raw text), so the module has to reproduce the model's trained
    // dialect byte for byte. Get it wrong and the model still generates --
    // it just generates against a prompt shape it never saw, which reads as
    // fluent garbage, or as a reply that never stops because the stop token
    // it learned never appears.
    //
    // Names match both HearthsideChat.LLM.Template and llama.cpp's own
    // --chat-template names for the same four formats, so an operator who
    // knows which flag llama-server needs already knows what to put here.
    // ------------------------------------------------------------------
    enum class HsRole { System, User, Assistant };

    enum class HsDialect
    {
        Llama3,   // <|start_header_id|>role<|end_header_id|>\n\ncontent<|eot_id|>
        ChatMl,   // <|im_start|>role\ncontent<|im_end|>\n
        Mistral,  // [INST] user [/INST] assistant</s>
        Gemma,    // <start_of_turn>role\ncontent<end_of_turn>\n
    };

    HsDialect ResolveDialect(const std::string& kind)
    {
        if (IEquals(kind, "chatml"))
            return HsDialect::ChatMl;
        if (IEquals(kind, "mistral"))
            return HsDialect::Mistral;
        if (IEquals(kind, "gemma"))
            return HsDialect::Gemma;
        return HsDialect::Llama3;  // also the fallback hs_config.cpp normalises to
    }

    // Gemma calls the assistant "model"; the other three agree on the names.
    const char* RoleName(HsDialect dialect, HsRole role)
    {
        if (role == HsRole::System)
            return "system";
        if (role == HsRole::User)
            return "user";
        return dialect == HsDialect::Gemma ? "model" : "assistant";
    }

    // Mistral and Gemma have no system role at all. Both upstream templates
    // handle that by folding system content into the following user turn, so
    // this does the same rather than inventing a system turn those models
    // never saw. Note what this costs: the archetype delta stops being its
    // own turn and joins the next user turn, so for these two dialects the
    // cache-shared prefix ends at the few-shot block.
    std::vector<std::pair<HsRole, std::string>> FoldSystemTurns(
        std::vector<std::pair<HsRole, std::string>> turns)
    {
        std::vector<std::pair<HsRole, std::string>> folded;
        std::string pending;
        for (auto& turn : turns)
        {
            if (turn.first == HsRole::System)
            {
                pending += (pending.empty() ? "" : "\n\n") + turn.second;
                continue;
            }
            if (turn.first == HsRole::User && !pending.empty())
            {
                folded.emplace_back(HsRole::User, pending + "\n\n" + turn.second);
                pending.clear();
                continue;
            }
            folded.push_back(turn);
        }
        // Unreachable today (the trigger is always the last turn and always a
        // user turn), but dropping operator-authored system text silently is
        // not an acceptable way to find that out.
        if (!pending.empty())
            folded.emplace_back(HsRole::User, pending);
        return folded;
    }

    // Renders the dialect-independent turn list into one dialect's markup.
    // No BOS token is emitted for any of the four (<|begin_of_text|>, <s>,
    // <bos>): llama-server's /completion adds it when it tokenizes the
    // prompt, and emitting it here would double it.
    std::string RenderPrompt(HsDialect dialect, std::vector<std::pair<HsRole, std::string>> turns)
    {
        if (dialect == HsDialect::Mistral || dialect == HsDialect::Gemma)
            turns = FoldSystemTurns(std::move(turns));

        std::string out;
        for (auto const& turn : turns)
        {
            switch (dialect)
            {
                case HsDialect::ChatMl:
                    out += std::string("<|im_start|>") + RoleName(dialect, turn.first) + "\n"
                            + turn.second + "<|im_end|>\n";
                    break;
                case HsDialect::Mistral:
                    out += turn.first == HsRole::User ? "[INST] " + turn.second + " [/INST]"
                                                       : " " + turn.second + "</s>";
                    break;
                case HsDialect::Gemma:
                    out += std::string("<start_of_turn>") + RoleName(dialect, turn.first) + "\n"
                            + turn.second + "<end_of_turn>\n";
                    break;
                case HsDialect::Llama3:
                default:
                    out += std::string("<|start_header_id|>") + RoleName(dialect, turn.first)
                            + "<|end_header_id|>\n\n" + turn.second + "<|eot_id|>";
                    break;
            }
        }

        // Open the assistant turn the model is being asked to complete.
        switch (dialect)
        {
            case HsDialect::ChatMl:
                out += "<|im_start|>assistant\n";
                break;
            case HsDialect::Mistral:
                break;  // the trailing [/INST] above already opens it
            case HsDialect::Gemma:
                out += "<start_of_turn>model\n";
                break;
            case HsDialect::Llama3:
            default:
                out += "<|start_header_id|>assistant<|end_header_id|>\n\n";
                break;
        }
        return out;
    }

    // "\n" is in every list for the same reason it always was: chat lines are
    // one line, and without it a model that reads the few-shot block as a
    // transcript keeps completing turns until n_predict chops it mid-sentence.
    json StopSequencesFor(HsDialect dialect)
    {
        switch (dialect)
        {
            case HsDialect::ChatMl:  return json::array({ "<|im_end|>", "\n" });
            case HsDialect::Mistral: return json::array({ "</s>", "\n" });
            case HsDialect::Gemma:   return json::array({ "<end_of_turn>", "\n" });
            case HsDialect::Llama3:
            default:                 return json::array({ "<|eot_id|>", "\n" });
        }
    }

    std::string BaseUrlNoTrailingSlash(std::string url)
    {
        if (!url.empty() && url.back() == '/')
            url.pop_back();
        return url;
    }

    // A backend-agnostic proxy for assembled prompt length (§4.19/§4.2):
    // sums the same content regardless of which apiType branch below turns
    // it into a raw string (llamacpp) or a messages array (ollama/openai),
    // so the metric is comparable across backends.
    uint32_t PromptCharCount(const std::string& systemPrompt, const std::string& archetypeLine,
                              const std::vector<HsHistoryTurn>& history, const std::string& trigger)
    {
        size_t total = systemPrompt.size() + archetypeLine.size() + trigger.size();
        for (auto const& ex : Fewshot())
            total += ex.first.size() + ex.second.size();
        for (auto const& turn : history)
            total += turn.trigger.size() + turn.reply.size();
        return static_cast<uint32_t>(total);
    }

    HsLLMFailure ClassifyTransportError(hs_httplib::Error err)
    {
        switch (err)
        {
            case hs_httplib::Error::ConnectionTimeout:
            case hs_httplib::Error::Timeout:
            case hs_httplib::Error::Read:
            case hs_httplib::Error::Write:
                return HsLLMFailure::Timeout;
            default:
                return HsLLMFailure::ConnectionFailed;
        }
    }

    // Holds a persistent keep-alive client rather than constructing one per
    // request. Cached thread_local, keyed by host:port. Safe without
    // locking because the worker pool is fixed at exactly one thread, so
    // exactly one thread ever calls this.
    hs_httplib::Client& GetPlainClient(const std::string& host, int port, int timeoutSec)
    {
        static thread_local std::string s_key;
        static thread_local std::unique_ptr<hs_httplib::Client> s_client;

        std::string key = host + ":" + std::to_string(port);
        if (!s_client || s_key != key)
        {
            s_client = std::make_unique<hs_httplib::Client>(host, port);
            s_client->set_keep_alive(true);
            s_key = key;
        }
        s_client->set_connection_timeout(timeoutSec);
        s_client->set_read_timeout(timeoutSec);
        s_client->set_write_timeout(timeoutSec);
        return *s_client;
    }

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    hs_httplib::SSLClient& GetSSLClient(const std::string& host, int port, int timeoutSec)
    {
        static thread_local std::string s_key;
        static thread_local std::unique_ptr<hs_httplib::SSLClient> s_client;

        std::string key = host + ":" + std::to_string(port);
        if (!s_client || s_key != key)
        {
            s_client = std::make_unique<hs_httplib::SSLClient>(host, port);
            s_client->enable_server_certificate_verification(false);
            s_client->set_keep_alive(true);
            s_key = key;
        }
        s_client->set_connection_timeout(timeoutSec);
        s_client->set_read_timeout(timeoutSec);
        s_client->set_write_timeout(timeoutSec);
        return *s_client;
    }
#endif

    struct HsHttpOutcome
    {
        std::string  body;
        HsLLMFailure failure;
        int          httpStatus;
    };

    HsHttpOutcome HttpPost(const std::string& url,
                            const std::string& body,
                            const std::vector<std::pair<std::string, std::string>>& extraHeaders,
                            int timeoutSec)
    {
        static const std::regex urlRe(R"(^(https?)://([^:/]+)(?::(\d+))?(/.*)?$)");
        std::smatch m;
        if (!std::regex_match(url, m, urlRe))
        {
            LOG_ERROR("module.hearthside.llm", "[HearthsideChat] Invalid LLM URL: {}", url);
            return { "", HsLLMFailure::ConnectionFailed, 0 };
        }

        std::string proto = m[1].str();
        std::string host  = m[2].str();
        std::string path  = m[4].matched ? m[4].str() : "/";
        int port = proto == "https" ? 443 : 80;
        if (m[3].matched)
        {
            // The regex accepts \d+ of any length, so std::stoul throws
            // out_of_range on an overlong digit run. This runs on the
            // queue-worker and generator threads, where an escaping exception
            // is std::terminate. LLM.Url/Generator.LLM.Url are both
            // operator-editable and live-reloading, so one conf typo must
            // degrade to a logged failure, not take the worldserver down.
            try
            {
                unsigned long parsed = std::stoul(m[3].str());
                if (parsed == 0 || parsed > 65535)
                    throw std::out_of_range("port out of range");
                port = static_cast<int>(parsed);
            }
            catch (const std::exception&)
            {
                LOG_ERROR("module.hearthside.llm", "[HearthsideChat] Invalid port in LLM URL: {}", url);
                return { "", HsLLMFailure::ConnectionFailed, 0 };
            }
        }

        hs_httplib::Headers headers = { {"Accept", "application/json"}, {"User-Agent", "mod-hearthside-chat/1.0"} };
        for (auto const& h : extraHeaders)
            headers.emplace(h.first, h.second);

        hs_httplib::Result res;
        if (proto == "https")
        {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
            hs_httplib::SSLClient& cli = GetSSLClient(host, port, timeoutSec);
            res = cli.Post(path, headers, body, "application/json");
#else
            LOG_ERROR("module.hearthside.llm", "[HearthsideChat] HTTPS requested but OpenSSL not compiled in.");
            return { "", HsLLMFailure::ConnectionFailed, 0 };
#endif
        }
        else
        {
            hs_httplib::Client& cli = GetPlainClient(host, port, timeoutSec);
            res = cli.Post(path, headers, body, "application/json");
        }

        if (!res)
        {
            HsLLMFailure failure = ClassifyTransportError(res.error());
            LOG_ERROR("module.hearthside.llm", "[HearthsideChat] LLM request failed for {}:{}{} — {}",
                host, port, path, failure == HsLLMFailure::Timeout ? "timeout" : "connection failed");
            return { "", failure, 0 };
        }
        if (res->status != 200)
        {
            HsLLMFailure failure = res->status >= 500 ? HsLLMFailure::ServerError : HsLLMFailure::ClientError;
            LOG_ERROR("module.hearthside.llm", "[HearthsideChat] LLM HTTP {} from {}:{}{} — {}",
                res->status, host, port, path, failure == HsLLMFailure::ServerError ? "backend error" : "our bug (malformed request?)");
            return { "", failure, res->status };
        }
        return { res->body, HsLLMFailure::None, res->status };
    }
}

HsLLMResult Hs_CallLLM(const HsLLMConfig& cfg, const std::string& systemPrompt,
                        const std::string& archetypeLine,
                        const std::vector<HsHistoryTurn>& history, const std::string& trigger)
{
    HsLLMResult result{ false, "", HsLLMFailure::None, 0, 0, 0 };
    result.promptChars = PromptCharCount(systemPrompt, archetypeLine, history, trigger);

    const bool isLlamaCpp = IEquals(cfg.apiType, "llamacpp");
    const bool isOllama   = IEquals(cfg.apiType, "ollama");

    std::string url = BaseUrlNoTrailingSlash(cfg.baseUrl);
    json body;
    std::vector<std::pair<std::string, std::string>> headers;

    // Sampler profile: min_p alone beat both Qwen's and Meta's recommended
    // profiles on measured opener diversity. These must travel in the
    // request body rather than rely on whatever the operator last typed on
    // the llama-server command line.
    if (isLlamaCpp)
    {
        url += "/completion";

        // Native /completion bypasses the server's chat template entirely,
        // so the module must reproduce the model's dialect itself.
        // cfg.templateKind (HearthsideChat.LLM.Template /
        // Generator.LLM.Template) picks which one, since the reactive and
        // generator endpoints can point at differently-tuned models.
        //
        // Layer order matters for prompt caching: system rules, then the
        // fixed few-shot block (byte-identical for every bot, this whole
        // prefix is what the server reuses), then the archetype delta
        // (byte-identical across bots sharing an archetype, but not across
        // all bots, so it sits after the truly-shared prefix rather than
        // inside it), then this bot-player pair's history as real prior
        // turns so a later turn's prompt is a strict byte extension of an
        // earlier one, then the new trigger.
        //
        // That order is dialect-independent, so it is built once
        // as role/content turns and rendered by RenderPrompt above.
        std::vector<std::pair<HsRole, std::string>> turns;
        turns.emplace_back(HsRole::System, systemPrompt);
        for (auto const& ex : Fewshot())
        {
            turns.emplace_back(HsRole::User, ex.first);
            turns.emplace_back(HsRole::Assistant, ex.second);
        }
        if (!archetypeLine.empty())
            turns.emplace_back(HsRole::System, archetypeLine);
        for (auto const& h : history)
        {
            turns.emplace_back(HsRole::User, h.trigger);
            turns.emplace_back(HsRole::Assistant, h.reply);
        }
        turns.emplace_back(HsRole::User, trigger);

        const HsDialect dialect = ResolveDialect(cfg.templateKind);
        std::string prompt        = RenderPrompt(dialect, std::move(turns));
        json        stopSequences = StopSequencesFor(dialect);

        body["prompt"]       = prompt;
        body["n_predict"]    = cfg.maxTokens;
        body["temperature"]  = 1.0;
        body["top_p"]        = 1.0;
        body["top_k"]        = 0;
        body["min_p"]        = 0.05;
        body["cache_prompt"] = true;
        body["stop"]         = stopSequences;

        // Only meaningful now that history gives DRY a cross-turn window to
        // look back through (dry_penalty_last_n: 64, never -1, that
        // setting suppressed every continuation outright). Off by default
        // (dryMultiplier 0.0) since it costs ~85ms/reply.
        if (cfg.dryMultiplier > 0.0f)
        {
            body["dry_multiplier"]     = cfg.dryMultiplier;
            body["dry_base"]           = 1.75;
            body["dry_allowed_length"] = 2;
            body["dry_penalty_last_n"] = 64;
        }

        if (!cfg.apiKey.empty())
            headers.emplace_back("Authorization", "Bearer " + cfg.apiKey);
    }
    else if (isOllama)
    {
        url += "/api/chat";

        json messages = json::array();
        if (!systemPrompt.empty())
            messages.push_back({ {"role", "system"}, {"content", systemPrompt} });
        for (auto const& ex : Fewshot())
        {
            messages.push_back({ {"role", "user"}, {"content", ex.first} });
            messages.push_back({ {"role", "assistant"}, {"content", ex.second} });
        }
        if (!archetypeLine.empty())
            messages.push_back({ {"role", "system"}, {"content", archetypeLine} });
        for (auto const& turn : history)
        {
            messages.push_back({ {"role", "user"}, {"content", turn.trigger} });
            messages.push_back({ {"role", "assistant"}, {"content", turn.reply} });
        }
        messages.push_back({ {"role", "user"}, {"content", trigger} });

        body["model"]    = cfg.model;
        body["messages"] = messages;
        body["stream"]   = false;
        body["options"]  = { {"temperature", 1.0}, {"top_p", 1.0}, {"top_k", 0}, {"min_p", 0.05}, {"num_predict", cfg.maxTokens} };

        if (!cfg.apiKey.empty())
            headers.emplace_back("Authorization", "Bearer " + cfg.apiKey);
    }
    else // "openai", also the shape llama-server's /v1/chat/completions accepts
    {
        url += "/chat/completions";

        json messages = json::array();
        if (!systemPrompt.empty())
            messages.push_back({ {"role", "system"}, {"content", systemPrompt} });
        for (auto const& ex : Fewshot())
        {
            messages.push_back({ {"role", "user"}, {"content", ex.first} });
            messages.push_back({ {"role", "assistant"}, {"content", ex.second} });
        }
        if (!archetypeLine.empty())
            messages.push_back({ {"role", "system"}, {"content", archetypeLine} });
        for (auto const& turn : history)
        {
            messages.push_back({ {"role", "user"}, {"content", turn.trigger} });
            messages.push_back({ {"role", "assistant"}, {"content", turn.reply} });
        }
        messages.push_back({ {"role", "user"}, {"content", trigger} });

        body["model"]       = cfg.model;
        body["messages"]    = messages;
        body["max_tokens"]  = cfg.maxTokens;
        body["temperature"] = 1.0;
        body["top_p"]       = 1.0;
        body["stream"]      = false;

        if (!cfg.apiKey.empty())
            headers.emplace_back("Authorization", "Bearer " + cfg.apiKey);
    }

    auto callStart = std::chrono::steady_clock::now();
    HsHttpOutcome outcome = HttpPost(url, body.dump(), headers, cfg.timeoutSec);
    result.latencyMs = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - callStart).count());
    result.httpStatus = outcome.httpStatus;
    if (outcome.failure != HsLLMFailure::None)
    {
        result.failure = outcome.failure;
        return result;
    }

    try
    {
        json resp = json::parse(outcome.body);

        if (resp.contains("error"))
        {
            std::string errMsg = resp["error"].is_object() && resp["error"].contains("message")
                ? resp["error"]["message"].get<std::string>()
                : outcome.body;
            LOG_ERROR("module.hearthside.llm", "[HearthsideChat] LLM API error: {}", errMsg);
            result.failure = HsLLMFailure::ParseError;
            return result;
        }

        std::string text;
        if (isLlamaCpp)
        {
            if (!resp.contains("content"))
            {
                LOG_ERROR("module.hearthside.llm", "[HearthsideChat] Unexpected llama.cpp /completion response shape.");
                result.failure = HsLLMFailure::ParseError;
                return result;
            }
            text = resp["content"].get<std::string>();
        }
        else if (isOllama)
        {
            if (!resp.contains("message") || !resp["message"].contains("content") ||
                !resp["message"]["content"].is_string())
            {
                LOG_ERROR("module.hearthside.llm", "[HearthsideChat] Unexpected Ollama response shape.");
                result.failure = HsLLMFailure::ParseError;
                return result;
            }
            text = resp["message"]["content"].get<std::string>();
        }
        else
        {
            // Review C7: the llamacpp and Ollama branches above check every
            // level of their shape explicitly; this one checked only the
            // outer array and then indexed ["message"]["content"] blind.
            // resp is non-const, so operator[] on a missing key *inserts a
            // null* and get<std::string>() throws type_error, caught by the
            // handler below -- the right outcome by the wrong route, and it
            // logs "JSON parse error" for what is actually a shape
            // mismatch. Checked explicitly so the three branches read the
            // same and the log names the real problem.
            if (!resp.contains("choices") || resp["choices"].empty() ||
                !resp["choices"][0].contains("message") ||
                !resp["choices"][0]["message"].contains("content") ||
                !resp["choices"][0]["message"]["content"].is_string())
            {
                LOG_ERROR("module.hearthside.llm", "[HearthsideChat] Unexpected OpenAI-compatible response shape.");
                result.failure = HsLLMFailure::ParseError;
                return result;
            }
            text = resp["choices"][0]["message"]["content"].get<std::string>();
        }

        text = CollapseNewlines(Trim(text));
        text = StripWrappingQuotes(text);
        text = Trim(text);

        result.success = !text.empty();
        result.text    = text;
        if (!result.success)
            result.failure = HsLLMFailure::ParseError;
        return result;
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("module.hearthside.llm", "[HearthsideChat] LLM response JSON parse error: {}", ex.what());
        result.failure = HsLLMFailure::ParseError;
        return result;
    }
}
