#ifndef MOD_HS_RAG_H
#define MOD_HS_RAG_H

#include <cstdint>
#include <string>
#include <vector>

// Static world-knowledge retrieval for the reactive (LLM) tier: the one
// grounding layer this module was missing.
//
// hs_grounded.h answers from live Player*/DB state, hs_topic_gate.h states
// live facts about the bot itself, hs_memory.h recalls a specific player.
// None of them know anything about Azeroth. So "where do I train
// blacksmithing" reaches the backend with nothing but the persona line, and
// a 1-3B local model invents an answer. This file closes that: it matches a
// player's message against a table of authored WotLK facts and hands the
// caller a short reference block to append to the prompt, the same way
// Hs_TopicGateLine's output is appended.
//
// Pure logic, no AzerothCore dependency, standalone-testable -- same split
// as hs_topic_gate.h/hs_grounded.h. The SQL load that fills the table lives
// in hs_rag_store.cpp (not written yet; Tests/test_hs_rag.cpp seeds the
// table straight from data/rag/*.json instead).
//
// Retrieval note, because the obvious implementation does not work: this is
// deliberately NOT cosine similarity over a corpus-wide term-frequency
// vector, which is what mod-ollama-chat's RAG does. That approach divides by
// the entry's own length, so a 5-word chat question against a 55-word entry
// scores ~0.09 and never clears a sane threshold -- it retrieves nothing on
// realistic chat lines. Here the score is normalized by the *query's*
// information mass instead, so it reads as "what fraction of what the player
// asked about does this entry actually cover", independent of entry length.

struct HsRagEntry
{
    std::string              id;       // stable identifier, also the sort tie-break
    std::string              title;    // short label, used in the prompt block
    std::string              content;  // the fact paragraph handed to the model
    std::vector<std::string> keywords; // authored retrieval handles: the strongest signal
};

struct HsRagHit
{
    const HsRagEntry* entry; // points into the table held by Hs_SetRagTable

    // Query coverage in [0,1] -- the share of what the player asked about that
    // this entry covers -- plus small aboutness/phrase bonuses, so a strong hit
    // can exceed 1.0. Deliberately not clamped: clamping collapsed the best
    // hits into ties that then broke alphabetically, which is how "molten core"
    // once retrieved Tier Sets ahead of Molten Core.
    float              score;
};

// Replaces the whole in-memory table and rebuilds the inverted index.
// Called once at startup (and again on `.reload config`), same shape as
// Hs_SetGroundedQuestionTable. Every previously returned HsRagHit::entry
// dangles after this call; callers consume hits within one request, so
// there is no lifetime issue in practice.
void Hs_SetRagTable(const std::vector<HsRagEntry>& rows);

// Number of loaded entries; 0 means every retrieval will miss (DB not
// loaded, or the table came back empty), which degrades to "no reference
// block in the prompt" rather than to an error.
size_t Hs_RagEntryCount();

// Scores `query` (the player's message, exactly as typed) against every
// entry and returns at most `maxEntries` hits scoring >= `minScore`,
// best first. Ties break on id so the prompt prefix stays byte-stable
// across identical questions, which matters for the backend's prefix cache.
//
// Both bounds are caller-supplied rather than read from hs_config.h here,
// for the same reason HsStyleContext's fields are: hs_config.h pulls in
// AzerothCore's ScriptMgr.h and would end this file's standalone testability.
std::vector<HsRagHit> Hs_RetrieveRag(const std::string& query, uint32_t maxEntries, float minScore);

// Formats hits into the block appended to the persona line. Returns "" for
// no hits. Truncated to `maxChars` on an entry boundary where possible, so
// a large retrieval cannot blow the prompt budget; the first entry is
// word-truncated if it alone exceeds the cap.
//
// Phrased as a statement of what the bot knows, not as an instruction about
// what to do with it -- same reasoning as hs_topic_gate.h's fact lines.
std::string Hs_RagContextLine(const std::vector<HsRagHit>& hits, uint32_t maxChars);

#endif // MOD_HS_RAG_H
