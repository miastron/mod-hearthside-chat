#ifndef MOD_HS_ARBITER_H
#define MOD_HS_ARBITER_H

#include <string>
#include <vector>

class Player;

// Centralised reply arbitration for the "addressed" branch of the decision
// flow (a real player's /say, with one or more bots in range). Answers
// "who replies, and how many," never "should this bot reply" rolled
// independently per bot: that per-bot rolling is exactly the "four bots
// answer the same line" defect this exists to prevent.
//
// `speaker` is the player who spoke; `candidates` is the already-gated
// eligible set (real-player/faction/range/combat gating happens in the
// caller, hs_handler.cpp; the arbiter only selects among what it is
// given). Returns 0-2 bots. Archetype reply-chance is rolled here, dropping
// candidates before the reply count is picked; tier ceilings, the token
// bucket, and cooldowns are not touched, those are applied by the caller,
// after selection.
//
// The event surface has its own arbiter, Hs_ArbitrateEventReplies
// (hs_event_arbiter.h), rather than extra parameters here: an event has no
// text for the name short-circuit below to match against, its candidates
// are not interchangeable (the bot that died outranks one that watched),
// and its reply-count distribution is per-event rather than the flat
// 50/42/8 this function uses. That file is also genuinely
// AzerothCore-free, which this one is not (Player.h/Random.h below), so it
// carries a standalone harness this one cannot.
std::vector<Player*> Hs_ArbitrateReplies(Player* speaker, const std::string& message, const std::vector<Player*>& candidates);

// True when the line names one of the real players who heard it and none of
// the candidate bots: "thanks Bob", "Bob you coming?" said to a friend in
// /say, party or guild chat. People nearby don't answer a line addressed to
// someone else, so the caller lets no bot reply. Same whole-word,
// case-insensitive match as the named-address rule in Hs_ArbitrateReplies.
bool Hs_AddressesSomeoneElse(const std::string& message, const std::vector<Player*>& bots,
                             const std::vector<Player*>& humans);

#endif // MOD_HS_ARBITER_H
