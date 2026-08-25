#ifndef MOD_HS_EVENT_ARBITER_H
#define MOD_HS_EVENT_ARBITER_H

#include <cstdint>
#include <string>
#include <vector>

// Event-reply arbitration: "who reacts, and how many" when something
// *happens* (a death, a ding, a killing blow, a roll, a duel) rather than
// when someone speaks. The sibling of Hs_ArbitrateReplies (hs_arbiter.h),
// which answers the same question for a real player's /say.
//
// Why a separate file rather than a second function in hs_arbiter.cpp (as
// PLAN-ARBITER.md §2 originally proposed): that file includes Player.h and
// Random.h, so it is not standalone-testable, and §9's
// Tests/test_hs_event_arbiter.cpp harness could not exist there. Everything
// in here is pure -- no AzerothCore headers, no globals, no clock -- so the
// weighting and count distribution are verifiable on the workstation.
// hs_event.cpp is the AzerothCore-facing half that computes each
// candidate's inputs from live Player*s and fires the result.
//
// Three things the /say arbiter cannot express, and this one can:
//   1. Involvement -- event candidates are not interchangeable listeners.
//      The bot that died has different standing from one that watched.
//   2. Per-event archetype affinity -- a duel should draw the PvP
//      archetypes more often than TRADER (hside_event_affinity).
//   3. Per-event reply count -- the /say arbiter's flat 50/42/8 is wrong in
//      both directions across events: a ding almost always draws a "gz",
//      most deaths pass without comment, duels draw almost nothing.
//
// The name short-circuit (MentionsName) is deliberately absent: an event
// has no text, so matching a bot's name against it is meaningless. The
// /say path relies on that check; relying on it no-opping against an empty
// string would be a coincidence, not a design.

// The event vocabulary. Shared by three consumers that must agree:
// hside_event_affinity.event_type, Claude/finetune/matrix/event.txt's
// trigger slates, and the fire sites in hs_event.cpp. Adding an entry means
// extending kEventTypeNames and kEventCountBias in lockstep -- both are
// indexed by this enum's underlying value, not searched.
enum class HsEventType : uint8_t
{
    DeathInGroup,          // bot died while grouped, group not wiped
    DeathWipe,             // every member of the group is now dead
    DeathGroupPlayer,      // a real player in the bot's group died
    DeathSolo,             // bot died ungrouped, out in the world
    LevelUpSelf,           // the bot itself dinged
    LevelUpGroup,          // someone in the bot's group dinged
    KillingBlow,           // bot landed a killing blow on an enemy player
    RollWon,               // bot won a group roll on a rare-or-better drop
    RollLost,              // someone else won one this bot rolled on
    DuelStart,             // a duel involving the bot is starting
    DuelWon,               // bot won a duel
    DuelLost,              // bot lost a duel

    // Reserved -- the four existing corpus openers (hs_opener.cpp), named
    // here so hside_event_affinity and matrix/event.txt can carry rows for
    // them ahead of PLAN-ARBITER.md §8's undecided tier-2 migration. No
    // fire site in hs_event.cpp routes to these today; openers remain
    // tier-1 corpus under MaxTier.Openers.
    OpenerGroupFormed,
    OpenerRez,
    OpenerDungeonComplete,
    OpenerProximity,
};

constexpr size_t kHsEventTypeCount = 16;

// Stable uppercase key, e.g. "DEATH_IN_GROUP". This is the string written
// into hside_event_affinity.event_type and the trigger vocabulary the
// fine-tune matrix keys off, so it is API, not a log label.
const char* Hs_EventTypeName(HsEventType type);

// Reverse lookup for the SQL loader. Returns false (out untouched) on an
// unrecognized name rather than guessing.
bool Hs_EventTypeForName(const std::string& name, HsEventType& out);

// How close this bot stands to the thing that happened. A shared three-level
// scale rather than a per-event one (PLAN-ARBITER.md §8): every event's fire
// site can express itself in these terms, and one scale keeps the weights
// comparable across events, which a per-event scale would not.
enum class HsEventInvolvement : uint8_t
{
    Witness,  // saw it happen to someone else
    Affected, // it happened to their group, or they rolled on the item and lost
    Subject,  // it happened to them
};

// The per-candidate inputs, all resolved at the fire site on the world
// thread where the Player*s are legal to touch. Nothing in this struct
// needs a game object, which is what keeps this file pure.
struct HsEventCandidate
{
    uint64_t botGuid = 0;

    // Distance in yards from the event's origin. Ignored when sameMap is
    // false -- GetDistance() across unrelated coordinate spaces is
    // meaningless, not merely "far", so a cross-map candidate takes the
    // beyond-range floor instead (the same rule hs_arbiter.cpp's
    // ProximityWeight applies for party/raid chat).
    float distance = 0.0f;
    bool  sameMap  = true;

    // Hs_SecondsSinceLastReply(botGuid) (hs_queue.h). UINT32_MAX when the
    // bot has never spoken, which the recency curve reads as fully
    // recovered.
    uint32_t secondsSinceLastReply = UINT32_MAX;

    HsEventInvolvement involvement = HsEventInvolvement::Witness;

    // Hs_EventAffinityWeight for (this candidate's own event type, this
    // bot's archetype). Resolved at the fire site, not here, for the same
    // reason involvement is: it keeps the table lookup off the hot path of
    // the selection loop. Note "this candidate's own event type" -- for a
    // duel end the winner resolves against DUEL_WON and the loser against
    // DUEL_LOST inside a single arbitration pass (below).
    float affinityWeight = 1.0f;

    // Opaque to this file; hs_event.cpp stores the trigger text to send if
    // this candidate is the one selected. Carried through so a duel end can
    // arbitrate once over {winner, loser} and only *then* decide whether
    // the prompt says "you just won" or "you just lost" -- one hook call,
    // one pass, outcome resolved after selection (PLAN-ARBITER.md §2).
    std::string trigger;
};

// Per-event reply-count bias, replacing the /say arbiter's own weights
// (HearthsideChat.ReplyCount.* -- hs_config.h). Compiled constants here,
// not config: this tunes the illusion, not a GPU dial. Percentages out of
// 100; the chance of two speakers is whatever these two leave over, so they
// must sum to no more than 100.
struct HsEventCountBias
{
    uint8_t none;
    uint8_t one;
};

HsEventCountBias Hs_EventCountBiasFor(HsEventType type);

// The affinity table (hside_event_affinity), keyed by (event type,
// archetype enum_name). Default 1.0 for any pair with no row, so the SQL
// authors only the exceptions. A row with weight 0.0 is the "never speaks
// to this event" floor -- no separate negative mechanism is needed
// (PLAN-ARBITER.md §8), since a zero-weight candidate can never be drawn by
// the cumulative selection below.
//
// Storage lives here rather than in the store so this file stays the whole
// testable unit; hs_event_affinity_store.cpp owns the query and calls
// Hs_SetEventAffinityTable at startup and on `.reload config`.
struct HsEventAffinityRow
{
    HsEventType type;
    std::string archetypeName; // hside_archetype.enum_name
    float       weight;
};

void  Hs_SetEventAffinityTable(const std::vector<HsEventAffinityRow>& rows);
float Hs_EventAffinityWeight(HsEventType type, const std::string& archetypeName);

// Selects 0-2 candidates by index into `candidates`. The weighting is a
// four-way product:
//
//     ProximityWeight x RecencyWeight x InvolvementWeight x AffinityWeight
//
// Selection is without replacement, so a two-reply event never picks the
// same bot twice. Returns indices rather than the structs so the caller can
// reach back for the matching trigger text.
//
// `sayDistance` is g_HsSayDistance, passed in rather than read from
// hs_config.h -- that header is not dependency-free, and reading it here
// would cost this file its standalone build.
std::vector<size_t> Hs_ArbitrateEventReplies(HsEventCountBias bias, float sayDistance,
                                              const std::vector<HsEventCandidate>& candidates);

// Deterministic seeding for the standalone harness. Not called by the module
// at runtime, where the generator seeds itself from random_device once.
void Hs_SeedEventArbiterForTest(uint32_t seed);

#endif // MOD_HS_EVENT_ARBITER_H
