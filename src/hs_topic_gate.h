#ifndef MOD_HS_TOPIC_GATE_H
#define MOD_HS_TOPIC_GATE_H

#include <cstdint>
#include <string>

// §4.13's topic gating for the reactive (LLM) tier: gear, group membership/
// leadership, in-instance, gold, and zone -- the remaining items from
// PLAN.md §4.13 with no runtime consumer yet (level is already gated by
// archetype eligibility, hs_archetype.h; combat by the style pass's `care`
// offset, hs_style.h).
//
// Same technique used elsewhere in the module (hs_queue.cpp's
// RpgStatusHint, hs_corpus.h's placeholders): state true facts, never an
// instruction telling the model what to avoid. A fact like "you are not in
// a group" makes a false claim contradict the persona line on its own,
// rather than relying on a suppression rule the model has to obey. It's a
// read-only snapshot re-read fresh every request, since group/instance/
// gold/zone are as volatile as combat.
//
// Pure logic, no AzerothCore dependency -- split like hs_archetype.h/
// hs_identity.h so it's standalone-testable. The caller (hs_handler.cpp's
// TryDispatch, hs_engagement.cpp's TryFireFollowUp) reads live Player*/
// Group* state into this struct on the world thread, then it travels
// through HsQueuedRequest to the worker thread untouched.
struct HsTopicGateContext
{
    uint32_t    avgItemLevel = 0;      // gear
    bool        inGroup      = false;  // group membership
    bool        isGroupLeader = false; // group leadership; meaningless if !inGroup
    bool        inInstance   = false;  // dungeon or raid map
    std::string instanceName;          // empty unless inInstance
    uint32_t    goldCopper   = 0;      // Player::GetMoney(), copper units
    std::string zoneName;              // empty if unresolved (e.g. no AreaTableEntry)
};

// Builds the fact line appended to personaLine (hs_queue.cpp's WorkerLoop),
// after the archetype line and RPG status hint. Never empty -- every field
// in HsTopicGateContext always has a true value to state, positive or
// negative (unlike RpgStatusHint, which omits a line for a status too
// generic to be worth stating).
std::string Hs_TopicGateLine(const HsTopicGateContext& ctx);

#endif // MOD_HS_TOPIC_GATE_H
