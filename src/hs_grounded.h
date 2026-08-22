#ifndef MOD_HS_GROUNDED_H
#define MOD_HS_GROUNDED_H

#include <cstdint>
#include <string>

// A lookup-and-template path sitting between the reflex check and the tier
// ceiling -- six questions the realm can already answer truthfully from
// live Player* state, with no GPU work and no chance of invention.
//
// Split the same way hs_reflex.h is: this file is pure trigger-matching and
// template text, no AzerothCore dependency, standalone-testable. The actual
// Player*/DBC/GuildMgr lookups that resolve each kind to a fact string live
// in hs_handler.cpp (already AzerothCore-dependent), which calls back into
// Hs_BuildGroundedReply once it has the fact in hand.
//
// Six templates and a dispatch table, not a rule engine -- deliberately
// narrower than the mount example's "where that mount comes from":
// acore_world has no queryable source/origin field for an arbitrary item
// or spell, and mod-playerbots-characters' own equivalent
// (pbc_scene_helpers.cpp's mounted-status builder) reports only the
// mount's name for the same reason. Answering what the realm actually
// knows rather than inventing a provenance story keeps every reply
// truthful.

enum class HsGroundedKind
{
    None,
    Mount,
    Level,
    Zone,
    Guild,
    Profession,
    Gear,

    // card_facts. Unlike the six above, these three only resolve for a
    // carded (ring-3) bot; the caller (hs_handler.cpp) treats "no active
    // card" or "field empty" the same way it treats Mount's "not observably
    // mounted" -- fall through rather than force a non-invented lacks-line
    // for a bot with no fact sheet at all.
    CurrentGoal,
    PlayedSince,
    Alt,

    // hside_memory, looked up and templated the same way, never carried in
    // a prompt. hasFact for all three comes from hs_memory_store.h's
    // pair-scoped queries, not live Player* state, so hs_handler.cpp's
    // resolution for these three is a DB lookup rather than a game-object
    // read, same shape as the three card-facts kinds above.
    RecallMet,      // "do you remember me"
    RecallDungeon,  // "what did we run"
    RecallGrouped,  // "have we grouped before"
};

// Matches `trigger` (the player's message, exactly as typed) against the
// six grounded-question phrase tables. Whole-message, not substring -- same
// false-positive-is-worse-than-a-miss reasoning as hs_reflex.h.
HsGroundedKind Hs_MatchGroundedQuestion(const std::string& trigger);

// Wraps `fact` (the caller's already-resolved lookup result, e.g. "42",
// "Elwynn Forest", "mining (300)") in one of several phrasing variants for
// `kind`, chosen by hash(botGuid, trigger) -- seeded per message rather
// than per bot, same idiom as hs_reflex.h's Plain family.
//
// `hasFact` selects between the has/lacks template set for the three kinds
// with a true negative answer (Guild: "not in a guild"; Profession: "no
// profession yet"; Gear: the defensive "nothing equipped" case). Mount,
// Level and Zone are always resolvable once matched and ignore `hasFact`.
// `fact` is unused when `hasFact` is false.
//
// CurrentGoal/PlayedSince/Alt have no lacks template -- `hasFact=false`
// returns an empty string for them, same as an unmatched kind, so the
// caller's existing "empty reply -> fall through" handling covers "no
// active card" for free without a new branch.
std::string Hs_BuildGroundedReply(HsGroundedKind kind, bool hasFact, const std::string& fact,
                                    uint64_t botGuid, const std::string& trigger);

#endif // MOD_HS_GROUNDED_H
