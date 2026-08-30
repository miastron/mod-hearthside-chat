#ifndef MOD_HS_LLM_H
#define MOD_HS_LLM_H

#include <cstdint>
#include <string>
#include <vector>

// Distinguishable failure causes get distinguishable handling, not one
// generic "LLM error".
enum class HsLLMFailure
{
    None,
    ConnectionFailed,   // refused / unreachable / could not connect at all
    Timeout,             // connection established but no timely response
    ServerError,         // HTTP 5xx, backend reached and failed
    ClientError,         // HTTP 4xx, our bug: malformed request, bad model, wrong endpoint
    ParseError,          // 200 but the response body wasn't the shape expected
};

// Result of a single LLM completion call.
struct HsLLMResult
{
    bool         success;
    std::string  text;        // trimmed, single-line, quote-stripped reply
    HsLLMFailure failure;      // HsLLMFailure::None when success is true
    int          httpStatus;  // 0 if the request never got an HTTP response
    uint32_t     latencyMs;   // wall time of the backend call itself, set on every outcome (§4.19)
    uint32_t     promptChars; // assembled prompt length: §4.2's budget that fails silently, set
                               // regardless of outcome since the prompt was still built and sent
};

// Endpoint configuration for one LLM call. Populated from HearthsideChat.LLM.*
// config keys (see hs_config.h). The reactive path and the idle-time
// generator each get their own HsLLMConfig, so this struct carries
// everything a call needs rather than reading globals directly.
struct HsLLMConfig
{
    std::string apiType;      // "llamacpp" (native /completion), "openai", or "ollama"
    std::string baseUrl;      // llamacpp: server root, e.g. http://host:8080
                               // openai:   .../v1
                               // ollama:   server root
    std::string apiKey;       // optional bearer token (llama-server supports --api-key)
    std::string model;        // ignored by llamacpp native /completion, required by the others
    int         timeoutSec;
    int         maxTokens;    // output cap (a GPU budget, not a hard chop)
    float       dryMultiplier; // DRY penalty multiplier, 0.0f leaves DRY off (llamacpp only)

    // Which chat-markup dialect to hand-assemble for apiType=llamacpp's
    // native /completion (that endpoint bypasses llama.cpp's own template
    // handling entirely, so the module must speak the model's exact
    // dialect itself, see Hs_CallLLM). Ignored by openai/ollama, which
    // send a structured messages array and let the server apply whatever
    // template its own tokenizer_config declares.
    // "llama3" (default): <|start_header_id|>role<|end_header_id|>\n\ncontent<|eot_id|>
    // "chatml":            <|im_start|>role\ncontent<|im_end|>\n  (Qwen and others)
    std::string templateKind = "llama3";
};

// One earlier exchange between this bot and this player: the line it was
// replying to, and what it actually said back. The caller (hs_queue) owns
// storage; this is just the shape replayed as real prior turns so the
// shared prefix cache sees history growth as an append, not a rewrite.
struct HsHistoryTurn
{
    std::string trigger;
    std::string reply;
};

// Universal synchronous LLM call. Safe to call from any thread; touches no
// AzerothCore game objects, so callers may run it off the world thread and
// must marshal the result back before acting on it (see hs_handler.cpp).
//
// Holds a persistent keep-alive HTTP client internally: cached thread_local,
// keyed by host:port, and reused across calls made from the same thread.
// The worker pool is fixed at exactly one thread, so this gives one
// persistent connection for the module's whole reactive tier with no
// locking needed.
//
// archetypeLine (hs_archetype.h, Hs_ArchetypePromptLine) is the per-bot
// personality delta, a distinct layer from systemPrompt: systemPrompt and
// the fixed few-shot block are byte-identical across every bot regardless
// of archetype (the cache-shared prefix), and archetypeLine is what
// actually differs, so it's placed after the few-shot block and before
// history rather than folded into systemPrompt itself. Pass an empty
// string for no delta (e.g. the scripted-conversation path generates
// against the baseline alone).
HsLLMResult Hs_CallLLM(const HsLLMConfig& cfg,
                        const std::string& systemPrompt,
                        const std::string& archetypeLine,
                        const std::vector<HsHistoryTurn>& history,
                        const std::string& trigger);

#endif // MOD_HS_LLM_H
