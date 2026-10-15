#ifndef MOD_HS_CLOUD_FACT_H
#define MOD_HS_CLOUD_FACT_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// The pure half of the cloud-fact relay (hs_cloud.h, Claude/PLAN-CLOUD-FACTS.md):
// deciding whether a player's line is a fact question at all, reading the
// cloud model's one-line answer, the saved-answer store, and the per-account
// daily limit. No AzerothCore header, no clock, no lock: hs_cloud.cpp owns the
// thread, the time and the mutex, and Tests/test_hs_cloud_fact.cpp builds
// this file standalone.

// True when `text` is shaped like a request for a fact: a question mark or a
// question opener, at least two words, and not a question about the bot
// itself. "you see the game last night?" and "are you a dog or cat person?"
// are small talk addressed to the bot, which the tuned model answers in
// character on its own; sending them out would only buy an "unknown". A
// second-person line still counts when it asks the bot to *know* something
// ("do you know the capital of peru").
//
// Says nothing about whether the question is about WoW: that is the caller's
// other test (Hs_RagHasGameTerm, hs_rag.h), which needs the live RAG table.
bool Hs_CloudIsFactQuestion(const std::string& text);

// True when `text` asks for game help without being shaped like a question:
// "ret pally or prot for leveling", "i keep getting ganked in stv, tips",
// "i lost a ton of durability from wiping, where to repair". A choice, a help
// word or a question phrase anywhere, three words or more, and, as above, not
// addressed to the bot. Used only for the game fallback (hs_cloud.h), after
// the router has said the line is about the game: on its own it would take
// "where to eat tonight" too.
bool Hs_CloudIsHelpRequest(const std::string& text);

// The cloud model's answer, read. The instruction asks for exactly one line,
// "Title -- One plain sentence. [days:N]", or "unknown". With web search on it
// tends to wrap that line ("Based on the search results, the answer is: ..."),
// so the parser looks for the " -- " separator and keeps only the title words
// after any lead-in ending in ':'.
struct HsCloudReply
{
    bool        known = false; // false: unknown, empty, or not in the expected shape
    std::string fact;          // "Title -- sentence." in the RAG block's own entry format
    uint32_t    days  = 0;     // the model's [days:N] label; 0 = none given
};
HsCloudReply Hs_ParseCloudReply(const std::string& raw);

// Lowercased content words of a question, for the saved-answer match:
// question scaffolding and pronouns dropped, a trailing plural 's' folded.
std::vector<std::string> Hs_CloudFactTerms(const std::string& text);

// True when the question names a relative day ("today", "last night",
// "tomorrow"): its answer is only true for the day it was asked, whatever
// expiry the model gave it. hs_cloud.cpp caps such an entry at a few hours.
bool Hs_CloudMentionsRelativeDay(const std::string& text);

struct HsCloudFactEntry
{
    std::string question;  // as the player typed it
    std::string fact;      // the parsed fact line
    uint64_t    expiresAt; // unix seconds
    bool        searched;  // answered with a web search
};

// Saved answers, matched by word overlap so a rephrased question finds the
// answer to the original. Dice coefficient over Hs_CloudFactTerms, at least
// kMatchMin, and every number in either question must appear in the other:
// "who won the 2026 world cup" must never be answered with 2022's winner.
class HsCloudFactStore
{
public:
    static constexpr float kMatchMin = 0.8f;

    // Replaces an entry with exactly the same terms, so a re-asked question
    // after expiry refreshes rather than duplicates.
    void Put(const HsCloudFactEntry& entry);

    // The best unexpired match, or nullptr. The pointer is valid until the
    // next Put or PruneExpired.
    HsCloudFactEntry const* Find(const std::string& question, uint64_t now) const;

    // Drops expired entries; returns how many.
    size_t PruneExpired(uint64_t now);

    size_t Size() const { return _entries.size(); }

private:
    struct Stored
    {
        HsCloudFactEntry         entry;
        std::vector<std::string> terms;
    };
    std::vector<Stored> _entries;
};

// Per-account count for one day. `day` is any number that changes once a day
// (hs_cloud.cpp passes the server-local date), so a new day resets every
// account without a sweep.
class HsCloudDailyLimiter
{
public:
    // Counts one call and returns true when `account` is under `limit` today;
    // returns false and counts nothing when it is not. limit 0 = no calls.
    bool TryTake(uint32_t account, uint32_t day, uint32_t limit);

    uint32_t Used(uint32_t account, uint32_t day) const;

private:
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> _counts; // account -> (day, count)
};

#endif // MOD_HS_CLOUD_FACT_H
