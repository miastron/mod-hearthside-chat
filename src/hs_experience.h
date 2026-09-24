#ifndef MOD_HS_EXPERIENCE_H
#define MOD_HS_EXPERIENCE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Ambient experience: what a bot has been *doing* lately, folded into its
// next prompt as background rather than fired as a reaction.
//
// This is deliberately not a tier and deliberately not an event trigger.
// hs_event.h already turns things that happen into things bots say, and
// Hs_EventCountBiasFor exists mostly to make that *not* fire ("most deaths
// pass without comment"). Widening that vocabulary would have produced more
// announcements, and an announcement per event is the tell that gives a bot
// away. An event is a backdrop, not a topic: a bot that just wiped twice in
// Sholazar should sound like it, not say so.
//
// So this file is a context layer beside hs_topic_gate.h (facts about the
// bot right now) and hs_rag.h (facts about Azeroth), holding facts about the
// bot's recent past. Those three, plus the archetype line, the card voice
// block, the RPG status hint and the recent-utterance echo, are all
// concatenated into the one personaLine that hs_queue.cpp's WorkerLoop
// hands to Hs_CallLLM -- this block is appended last, which is deliberate:
// see that assembly site for why position matters to the prefix cache.
//
// **In memory only, and that is the design, not a shortcut.** hside_memory
// is durable because a relationship should outlive a restart; "recently" is
// the opposite kind of fact. A worldserver restart losing it is correct, and
// it buys no table, no schema, no migration, and no eviction sweep. The
// noisy hooks that feed this (loot, money, skill ticks) would also be a
// write firehose against a real table with hundreds of bots.
//
// Pure logic, no AzerothCore dependency, standalone-testable -- same split as
// hs_rag.h/hs_rag_store.cpp and hs_memory.h/hs_memory_store.h. The buffer
// itself lives here rather than in the store half (contrast g_RecentUtterances,
// which sits inside hs_queue.cpp) specifically so the harness can exercise
// record-then-read round trips, collapsing, decay and eviction with no
// database and no game. hs_experience_store.h is only the hooks.

// The vocabulary. Chosen for what a player's recent activity actually
// colours their chat with, and pruned hard: every entry here has to survive
// the question "would knowing this change how someone sounds two minutes
// later?" Instance binds are absent because HsTopicGateContext::instanceName
// already states the same thing more precisely, and achievements because
// playerbots earn them rarely enough that the hook would idle.
enum class HsExperienceKind : uint8_t
{
    QuestCompleted, // subject: the quest title
    QuestAbandoned, // subject: the quest title
    LootedItem,     // subject: the item name, quality-filtered at the hook
    SkillUp,        // subject: "375 Skinning", milestone-filtered at the hook
    LevelUp,        // subject: the new level, as text
    MoneyGained,    // subject: gold amount as text, threshold-filtered at the hook
    ZoneEntered,    // subject: the zone name
    Died,           // subject: the zone it happened in
    DuelWon,        // subject: the opponent's name
    DuelLost,       // subject: the opponent's name
    PartyJoined,    // subject: "a party" -- membership, not an event about anyone

    // The three above were added 2026-09-20 for a failure the other eight
    // could not prevent. hs_event.cpp has had duel hooks all along, but those
    // only fire a proactive line at the moment the duel ends; nothing put the
    // outcome anywhere a *reply* could see it, and this ring is what reply
    // prompts read (hs_queue.cpp). A bot that had just lost a duel answered
    // "bwahahaha" with "ur not even trying" -- a winner's taunt -- and
    // answered "you're terrible" with "you're not helping", which frames a
    // duel as group content (realm 2026-09-20, hside_chat_log ids 1-3). It
    // was not guessing badly; it had nothing to guess from.
    //
    // PartyJoined's subject is the fixed string "a party" rather than a
    // roster or an inviter name: the useful fact is membership, not who else
    // is in it, and a roster would go stale inside the ring's own window.
    // It is not empty because Hs_RecordExperience drops empty subjects by
    // contract, and because the fixed value is what lets the ring collapse
    // repeat joins into `count` instead of stacking them.
    // It sits in a ring that decays rather than in durable
    // state on purpose: "just joined a party" stops being worth saying on its
    // own, which is exactly what windowSeconds already does. Repeats collapse
    // into `count` like every other kind, so a bot rejoining the same group
    // does not stack entries.
};

constexpr size_t kHsExperienceKindCount = 11;

// Distinct (kind, subject) pairs held per bot. Small on purpose: this block
// is per-request prompt weight on a T1000, and the fine-tune's whole goal is
// shrinking that segment (Claude/finetune/README.md). Six gives the renderer
// something to choose from while MaxEntries decides what actually ships.
constexpr size_t kHsExperienceCapPerBot = 6;

// Handed to HsPrune::PruneStale by the record path, in the same
// opportunistic, size-gated shape hs_queue.cpp's two maps use. Generous
// relative to the render window: pruning early would change behaviour,
// pruning late only delays reclamation.
constexpr int64_t kHsExperienceStaleSeconds   = 7200;
constexpr size_t  kHsExperiencePruneThreshold = 256;

// One remembered thing, with repeats collapsed rather than stacked.
//
// `count` is what makes this structure answer both halves of the problem at
// once. A bot that dies four times in one zone does not deserve four ring
// slots, and "died in Sholazar Basin (4 times)" is a better prompt line than
// any one of them -- so the counter that makes the noisy hooks survivable is
// also the aggregate that makes them interesting.
struct HsExperienceEntry
{
    HsExperienceKind kind          = HsExperienceKind::QuestCompleted;
    std::string      subject;
    uint32_t         count         = 1;
    int64_t          lastAtSeconds = 0; // monotonic; see Hs_ExperienceNowSeconds
};

// Stable lowercase key, e.g. "quest_completed". Used by the GM command and
// the harness; not written to any table, so unlike Hs_EventTypeName this is
// a label rather than API.
const char* Hs_ExperienceKindName(HsExperienceKind kind);

// Monotonic seconds, offsettable by the test hook below. Exposed because the
// renderer takes an explicit `now` so it stays a pure function of its inputs.
int64_t Hs_ExperienceNowSeconds();

// Records one thing this bot just did. An identical (kind, subject) already
// in the ring bumps its count and refreshes its timestamp instead of adding
// a row; otherwise the entry is appended and the least-recently-touched is
// evicted once the ring is over kHsExperienceCapPerBot.
//
// Empty `subject` is a no-op: every kind here names something, and a kind
// with nothing to name has nothing to contribute to a prompt. That is the
// same rule hs_corpus.h's placeholder resolvers apply -- an unresolvable
// fact drops the line rather than inventing a value.
//
// Safe to call from any thread. Called from the world thread in practice
// (hs_experience_store.cpp's hooks), where the Player* is legal to read.
void Hs_RecordExperience(uint64_t botGuid, HsExperienceKind kind, const std::string& subject);

// Formats entries into the block appended to personaLine. Pure: no lock, no
// map, no clock -- `now` is passed in. Returns "" for no renderable entry.
//
// Entries older than `windowSeconds` are skipped, the rest render most
// recent first, capped at `maxEntries` and truncated to `maxChars` on an
// entry boundary, same contract as Hs_RagContextLine.
//
// **Framed as background, not as material.** hs_rag.h learned this once
// already: kHsRagGeneratorPrefix says "for detail only -- do not quote or
// summarise it" because the reply-side phrasing invited the model to
// announce what it had been handed. A small model given a list of things it
// just did will read the list back unless told plainly that the list is not
// the subject. The 2026-09-23 wording keeps that half ("don't bring it up
// yourself") and drops the half that also stopped a bot answering when a
// player asked about it; the fine-tune carries rows for both (see
// Claude/finetune/add_context_layers.py).
std::string Hs_ExperienceLine(const std::vector<HsExperienceEntry>& entries, int64_t now,
                              uint32_t maxEntries, uint32_t maxChars, uint32_t windowSeconds);

// Retrieve and format in one call, under one lock -- the safe accessor, and
// the one production uses. Hands back a string rather than entries for the
// same reason hs_rag.h's one-shot accessors exist: the queue worker and the
// generator read this while the world thread's hooks are writing it, and a
// vector of entries handed across that boundary is a copy at best and a
// race at worst.
std::string Hs_ExperienceContext(uint64_t botGuid, uint32_t maxEntries, uint32_t maxChars, uint32_t windowSeconds);

// Snapshot of one bot's ring, most recent first. For the harness and for
// `.hearthside experience`; production prompt assembly uses the accessor
// above instead.
std::vector<HsExperienceEntry> Hs_ExperienceSnapshot(uint64_t botGuid);

// Drops everything for one bot. Called from Hs_ForgetBotHistory
// (hs_queue.h): a recycled bot has had its level, gear, zone and goals
// rewritten, so what it "did lately" is no longer true of the character
// standing there -- the same staleness argument that already drops its
// history and its recent utterances.
void Hs_ForgetExperience(uint64_t botGuid);

// Read-only counters for `.hearthside status`, the same visibility problem
// hs_opener.h's counter solves: this subsystem is invisible from a game
// client when it is working correctly.
uint32_t Hs_ExperienceTrackedBotCount();
uint32_t Hs_ExperienceRecordedThisSession();

// ---- test hooks; not called at runtime ----

// Shifts Hs_ExperienceNowSeconds forward without sleeping, so decay and
// window behaviour are testable in a harness.
void Hs_ExperienceAdvanceClockForTest(int64_t deltaSeconds);

// Clears every bot's ring and resets the clock offset and session counter.
void Hs_ExperienceResetForTest();

#endif // MOD_HS_EXPERIENCE_H
