#ifndef MOD_HS_ARBITER_H
#define MOD_HS_ARBITER_H

#include <string>
#include <vector>

class Player;

// PLAN.md §4.15 — centralised reply arbitration for the "addressed" branch of
// the decision flow (a real player's /say, with one or more bots in range).
// Answers "who replies, and how many," never "should this bot reply" rolled
// independently per bot — that per-bot rolling is exactly the "four bots
// answer the same line" defect this exists to prevent.
//
// `speaker` is the player who spoke; `candidates` is the already-gated
// eligible set (real-player/faction/range/combat gating happens in the
// caller, hs_handler.cpp — the arbiter only selects among what it is given).
// Returns 0-2 bots. Does not touch tier ceilings, the token bucket, or
// cooldowns — those are applied by the caller, after selection (§4.15 step 5).
std::vector<Player*> Hs_ArbitrateReplies(Player* speaker, const std::string& message, const std::vector<Player*>& candidates);

#endif // MOD_HS_ARBITER_H
