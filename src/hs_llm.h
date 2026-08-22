#ifndef MOD_HS_LLM_H
#define MOD_HS_LLM_H

#include <string>
#include <vector>

// PLAN.md §4.3 "the failure mode is named, not generic" — distinguishable
// causes get distinguishable handling, not one generic "LLM error".
enum class HsLLMFailure
{
    None,
    ConnectionFailed,   // refused / unreachable / could not connect at all
    Timeout,             // connection established but no timely response
    ServerError,         // HTTP 5xx — backend reached and failed
    ClientError,         // HTTP 4xx — our bug: malformed request, bad model, wrong endpoint
    ParseError,          // 200 but the response body wasn't the shape expected
};

// Result of a single LLM completion call.
struct HsLLMResult
{
    bool         success;
    std::string  text;        // trimmed, single-line, quote-stripped reply
    HsLLMFailure failure;      // HsLLMFailure::None when success is true
    int          httpStatus;  // 0 if the request never got an HTTP response
};

// Endpoint configuration for one LLM call. Populated from HearthsideChat.LLM.*
// config keys (see hs_config.h). PLAN.md §4.1: the reactive path and the
// (future) idle-time generator each get their own HsLLMConfig, so this struct
// carries everything a call needs rather than reading globals directly.
struct HsLLMConfig
{
    std::string apiType;      // "llamacpp" (native /completion), "openai", or "ollama"
    std::string baseUrl;      // llamacpp: server root, e.g. http://host:8080
                               // openai:   .../v1
                               // ollama:   server root
    std::string apiKey;       // optional bearer token (llama-server supports --api-key)
    std::string model;        // ignored by llamacpp native /completion, required by the others
    int         timeoutSec;
    int         maxTokens;    // output cap, §4.2 — a GPU budget, not a hard chop (§4.11)
    float       dryMultiplier; // §4.11 DRY retest — 0.0f leaves DRY off (llamacpp only)
};

// One earlier exchange between this bot and this player: the line it was
// replying to, and what it actually said back. PLAN.md §4.2 conversation
// history — the caller (hs_queue) owns storage; this is just the shape
// replayed as real prior turns so the shared prefix cache sees history
// growth as an append, not a rewrite.
struct HsHistoryTurn
{
    std::string trigger;
    std::string reply;
};

// Universal synchronous LLM call. Safe to call from any thread; touches no
// AzerothCore game objects, so callers may run it off the world thread and
// must marshal the result back before acting on it (see hs_handler.cpp).
//
// §4.1's "hold persistent keep-alive clients rather than constructing one
// per request" is implemented internally: the underlying HTTP client is
// cached thread_local, keyed by host:port, and reused across calls made
// from the same thread. Since PLAN.md §4.3 fixes the worker pool at exactly
// one thread, this gives one persistent connection for the module's whole
// reactive tier with no locking needed.
// archetypeLine (hs_archetype.h, Hs_ArchetypePromptLine) is the per-bot
// personality delta (§4.11 "baseline persona plus archetype delta"). It's a
// distinct layer from systemPrompt: systemPrompt + the fixed few-shot block
// are byte-identical across every bot regardless of archetype (that's the
// cache-shared prefix), and archetypeLine is what actually differs, so it
// is placed after the few-shot block and before history rather than folded
// into systemPrompt itself. Pass an empty string for no delta (e.g. the
// scripted-conversation path §4.16 generates against the baseline alone).
HsLLMResult Hs_CallLLM(const HsLLMConfig& cfg,
                        const std::string& systemPrompt,
                        const std::string& archetypeLine,
                        const std::vector<HsHistoryTurn>& history,
                        const std::string& trigger);

#endif // MOD_HS_LLM_H
