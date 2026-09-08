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
// in hs_rag_store.cpp; Tests/test_hs_rag.cpp bypasses it and seeds the table
// straight from data/rag/*.json, which is what keeps retrieval quality
// testable at corpus scale with no database.
//
// Retrieval note, because the obvious implementation does not work: this is
// deliberately NOT cosine similarity over a corpus-wide term-frequency
// vector, which is what mod-ollama-chat's RAG does. That approach divides by
// the entry's own length, so a 5-word chat question against a 55-word entry
// scores ~0.09 and never clears a sane threshold -- it retrieves nothing on
// realistic chat lines. Here the score is normalized by the *query's*
// information mass instead, so it reads as "what fraction of what the player
// asked about does this entry actually cover", independent of entry length.

// How a retrieved block opens. Two, because the consumers are two different
// kinds of thing: the reactive tier is prompting a character who is about to
// speak, the idle-time generator is prompting a writer producing a line to
// store. Telling the generator it "knows" something invites it to have the
// character announce it; telling it the text is source material gets a line
// that is merely *informed* by it, which is the whole point of grounding
// generation rather than replies.
constexpr char const* kHsRagReplyPrefix     = "Things you know about Azeroth: ";
constexpr char const* kHsRagGeneratorPrefix = "Accurate reference material about this game, "
                                              "for detail only -- do not quote or summarise it: ";

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
// Called once at startup (and again on `.reload config`) from the world
// thread by hs_rag_store.cpp, same shape as Hs_SetGroundedQuestionTable.
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
//
// SINGLE-THREADED CALLERS ONLY (the harness). Each HsRagHit::entry points
// into the live table and dangles the moment Hs_SetRagTable replaces it, so
// a worker- or generator-thread caller racing a `.reload config` would be
// reading freed memory, not stale data. Production callers use the two
// one-shot accessors below, which never let the pointer escape the lock.
std::vector<HsRagHit> Hs_RetrieveRag(const std::string& query, uint32_t maxEntries, float minScore);

// How many retrieval-bearing terms `query` actually carries: normalized,
// stopword-dropped, stemmed -- exactly the terms the scorer would weigh.
// Pure function of the string; no table, no lock.
//
// This exists because a *score* alone cannot tell a caller that a query was
// too thin to trust. Query mass is normalized, so a two-word line scores on
// the one term it has and scores it high: measured against the seed corpus,
// "good run." retrieves Stratholme at 0.820 and "nice, one down." retrieves
// Razorfen Downs at 0.647, both above anything a raised threshold could
// exclude without also discarding the genuine matches beneath them. Term
// count is the axis that separates them -- those two carry 1 and 2 terms
// against 4-6 for every correct hit in that range.
//
// A caller retrieving against a *player's* message does not need this: a
// player who types two words meant those two words. It is for a caller
// retrieving against generated text, where a short line is an artifact of the
// generator rather than a narrow request (hs_generator.cpp's script turns).
size_t Hs_RagQueryTermCount(const std::string& query);

// Formats hits into the block appended to the persona line. Returns "" for
// no hits. Truncated to `maxChars` on an entry boundary where possible, so
// a large retrieval cannot blow the prompt budget; the first entry is
// word-truncated if it alone exceeds the cap.
//
// Phrased as a statement of what the bot knows, not as an instruction about
// what to do with it -- same reasoning as hs_topic_gate.h's fact lines.
//
// `prefix` is what the block opens with, and it is a parameter because the
// two kinds of consumer need different framing: a reply prompt is addressed
// to a character ("things you know"), a generation prompt is addressed to a
// writer ("source material"). It counts against `maxChars`, so a longer
// prefix leaves less room for content rather than overrunning the budget.
std::string Hs_RagContextLine(const std::vector<HsRagHit>& hits, uint32_t maxChars,
                              const std::string& prefix = kHsRagReplyPrefix);

// Retrieve and format in one call, under one lock. The two-step
// Hs_RetrieveRag + Hs_RagContextLine pair above hands out raw pointers into
// the table; this does not, which is what makes it safe to call from the
// queue worker (hs_queue.cpp) and the generator (hs_generator.cpp) while the
// world thread may be replacing that table on `.reload config`.
//
// Returns "" when nothing clears `minScore`, which is the common case and
// degrades to "no reference block in the prompt".
std::string Hs_RagContextFor(const std::string& query, uint32_t maxEntries, float minScore, uint32_t maxChars,
                             const std::string& prefix = kHsRagReplyPrefix);

// Direct lookup: no scoring, no threshold, no near-miss. Each key is matched
// against entry ids first and then against normalized titles, and the first
// key that resolves wins.
//
// This is the accessor for a caller that already knows which paragraph it
// wants -- the generator filling a zone_tag bucket, an event fired inside a
// named instance -- rather than one guessing from a player's free text. It
// matters because the scored path's separation margin narrows as the corpus
// grows (data/rag/README.md), and a caller holding a name the *game* gave it
// should never be exposed to that: "Gundrak" is not a query, it is an
// address.
//
// Titles are authored as the thing a player would say ("Un'Goro Crater",
// "Gundrak", "Stormwind City"), so a zone name from AreaTable or a map name
// from Map::GetMapName can be passed straight through without the caller
// knowing the entry's id scheme.
std::string Hs_RagContextForKeys(const std::vector<std::string>& keys, uint32_t maxChars,
                                 const std::string& prefix = kHsRagReplyPrefix);

// One arbitrary entry, formatted the same way. No query, no key, no
// threshold: this is for a caller that has nothing to look anything up
// *with* and still wants the model working from a real fact.
//
// The generator's untagged buckets are why this exists. A zone_tag bucket
// addresses its own entry above, but chat_gripe_general, the two channel
// categories and the openers have no label at all, so before this they were
// prompted with no ground truth whatsoever and the model filled the gap by
// inventing game vocabulary. A random real paragraph is not "about" the
// bucket, but the generator prefix already frames it as detail rather than
// subject matter, and an ordinary player gripe informed by a real mechanic
// beats a fluent one about a mechanic that does not exist.
//
// The second caller is a bot-to-bot script's first turn, which is the same
// problem wearing different clothes: turn 1 replies to a fixed opening
// trigger, so there is no previous turn to retrieve against. Turns 2+ score
// the turn they are answering, and they answer a first turn that is now
// actually about something.
//
// `selector` is any value; it is reduced modulo the live entry count inside
// the lock. Callers therefore never read Hs_RagEntryCount() to pick an index,
// which would race a `.reload config` replacing the table between the two
// calls. Returns "" only when the table is empty.
std::string Hs_RagContextRandom(uint32_t selector, uint32_t maxChars,
                                const std::string& prefix = kHsRagReplyPrefix);

#endif // MOD_HS_RAG_H
