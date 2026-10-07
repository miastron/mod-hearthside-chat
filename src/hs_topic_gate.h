#ifndef MOD_HS_TOPIC_GATE_H
#define MOD_HS_TOPIC_GATE_H

#include <cstdint>
#include <string>

// §4.13's topic gating for the reactive (LLM) tier: who the bot is (level,
// race, class, guild, professions, riding), then gear, group, instance,
// gold and zone. Without the first part "what lvl are u" or "what class are
// you" reached the model with no true answer and it invented one, or read a
// near-empty item level as being new to the game (realm 2026-10-07).
//
// Same technique used elsewhere in the module (hs_queue.cpp's
// RpgStatusHint, hs_corpus.h's placeholders): state true facts, never an
// instruction telling the model what to avoid. A fact like "you are not in
// a group" makes a false claim contradict the persona line on its own,
// rather than relying on a suppression rule the model has to obey. It's a
// read-only snapshot re-read fresh every request, since group/instance/
// gold/zone are as volatile as combat.
//
// **Scoped to right now, deliberately.** Every field here is a fact about
// the present instant, which is what makes re-reading it per request the
// correct implementation. Facts about the bot's recent *past* are
// hs_experience.h's job and are stored rather than snapshotted, since
// nothing on a live Player* remembers that it wiped twice an hour ago. If a
// fact you want to add cannot be read off Player*/Group*/Map* in one call,
// it belongs there and not here.
//
// Pure logic, no AzerothCore dependency, split like hs_archetype.h/
// hs_identity.h so it's standalone-testable. The caller (hs_handler.cpp's
// TryDispatch, hs_engagement.cpp's TryFireFollowUp) reads live Player*/
// Group* state into this struct on the world thread, then it travels
// through HsQueuedRequest to the worker thread untouched.
struct HsTopicGateContext
{
    uint8_t     level        = 0;      // 0 = unknown: the identity part is left out
    std::string raceName;              // lowercase, e.g. "night elf"
    std::string className;             // lowercase, e.g. "death knight"
    std::string guildName;             // empty = no guild (stated by omission, never as a negative)
    std::string professions;           // e.g. "herbalism 120 and mining 95"; empty = none
    bool        canRide      = false;  // riding skill >= 75
    uint32_t    avgItemLevel = 0;      // gear
    bool        inGroup      = false;  // group membership
    bool        isGroupLeader = false; // group leadership; meaningless if !inGroup
    bool        inInstance   = false;  // dungeon or raid map
    std::string instanceName;          // empty unless inInstance
    uint32_t    goldCopper   = 0;      // Player::GetMoney(), copper units
    std::string zoneName;              // empty if unresolved (e.g. no AreaTableEntry)
};

// Builds the fact line appended to personaLine (hs_queue.cpp's WorkerLoop),
// after the archetype line and RPG status hint. Never empty. The identity
// facts lead because they change least (prompt-cache prefix); guild,
// professions and riding are stated only when present, so the model is
// never handed an invented negative.
std::string Hs_TopicGateLine(const HsTopicGateContext& ctx);

#endif // MOD_HS_TOPIC_GATE_H
