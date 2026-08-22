#ifndef MOD_HS_IDENTITY_STORE_H
#define MOD_HS_IDENTITY_STORE_H

#include <cstdint>
#include <string>

// PLAN.md §4.12 / §7 step 15 -- the DB-touching and mod-playerbots-touching
// half of identity rings. Pure logic (score weights, the promotion
// threshold, card-facts validation) lives in hs_identity.h; this file owns
// hside_identity reads/writes and the exclude-vector push into
// sPlayerbotAIConfig, same split as hs_gen_validate.h/hs_generator.h.

// §4.12 "count conversation, not groups": bumps this bot's interaction_score
// by `weight`, lazily creating the identity row on first score event (needs
// `botLevel` to fill the row's NOT NULL archetype/last_known_level columns --
// hs_archetype.h's Hs_ArchetypeForBot is pure GUID+level, so the snapshot
// costs nothing extra). Promotes (sets promoted_at) the instant the running
// total crosses kHsPromotionThreshold, if not already promoted. Card
// generation itself is the generator's job (hs_generator.h) picking up rows
// with promoted_at set and card_voice still NULL -- this function only flips
// the flag. Safe to call from any thread; only touches CharacterDatabase.
void Hs_BumpInteractionScore(uint64_t botGuid, uint8_t botLevel, uint32_t weight);

// One query's worth of what the reactive tier (hs_queue.cpp's WorkerLoop)
// and the delivery-side style pass need for a carded bot: the voice block
// (injected into the LLM prompt, §4.12 "the only card text that ever enters
// a prompt") and the verbal_tic (protected token, §4.11). `active` is false
// -- and the other two fields empty -- for a bot with no row or a dormant/
// retired card, which is the overwhelmingly common case.
struct HsCardSnapshot
{
    bool        active = false;
    std::string voiceBlock;
    std::string verbalTic;
};
HsCardSnapshot Hs_LookupCardSnapshot(uint64_t botGuid);

// One card_facts field, for the three grounded-answer questions this step
// wires up (current_goal, played_since, alt -- hs_grounded.h). Empty string
// (hasFact=false at the call site) when the bot has no active card or the
// field is absent -- same "fall through, don't fabricate a lacks-line" shape
// TryGrounded already uses for Mount.
std::string Hs_LookupCardFactField(uint64_t botGuid, const std::string& fieldName);

// Whether this bot currently has an active card -- hs_corpus.h's selection
// needs this to decide whether a card_gated category is eligible to draw
// from at all (§4.7: a card-gated line resolves only for a carded bot).
bool Hs_HasActiveCard(uint64_t botGuid);

// Re-applies every card_active bot's name into both of mod-playerbots'
// recycling-exclusion vectors (levelBracketsExcludeNames,
// resetBotLevelExcludeNames -- §4.13's "the exclusion levers already line up
// with the rings"). Idempotent (checks for an existing entry before
// appending), so it's safe to call repeatedly: at worldserver startup, on
// every `.reload config` (trap 16 -- playerbots' own OnAfterConfigLoad
// handler unconditionally clears both vectors from playerbots.conf, which
// has no way to encode these names), and every 300s from
// hs_main.cpp's HsIdentityLifecycleWorldScript::OnUpdate. The periodic call
// is not defensive insurance -- §4.13's own live test (run during step 15)
// found the cross-module OnAfterConfigLoad ordering unfavorable: playerbots'
// handler clears the vectors *after* this module's re-apply runs, so a
// carded bot's name is gone within one `.reload config` without it.
void Hs_ApplyExcludeVectorsFromIdentityTable();

// Pushes exactly one bot's name into both vectors -- called right after a
// card finishes generating (hs_generator.cpp), so the bot is protected
// immediately rather than waiting for the next startup/reload. Idempotent,
// same as the bulk version above.
void Hs_PushBotIntoExcludeVectors(uint64_t botGuid);

// The reverse of the above (§7 step 17): "exclusion tracks card_active, and
// is released on demotion" (§4.13). Called on both demotion (dormancy) and
// retirement (invalidation) -- either way the bot is no longer somebody's
// known bot and rejoins the recycling pool.
void Hs_RemoveBotFromExcludeVectors(uint64_t botGuid);

// §4.12 retirement (§7 step 17, trap 10): "a carded bot whose level drops is
// a retirement, not a repair." Clears both card halves, promoted_at and
// interaction_score, drops every hside_memory row for this bot (a retired
// bot must not quote shared history back at a level that contradicts it),
// redraws the archetype fresh for `newLevel`, and releases the bot's name
// from the recycling-exclusion vectors. The identity row itself survives --
// bot_guid and style_flags carry on, so the bot can earn a new card later as
// a new person. Called both from Hs_BumpInteractionScore (a level drop
// caught live, mid-conversation) and Hs_RunIdentityDailySweep (a carded,
// currently-online bot whose level changed while nobody was talking to it).
void Hs_RetireCard(uint64_t botGuid, uint8_t newLevel);

// §4.12 decay, pinning and retirement (§7 step 17). One daily sweep,
// intended to be driven from hs_main.cpp's HsIdentityLifecycleWorldScript
// (which already runs a periodic tick for the exclude-vector reconcile) --
// see that file for why this shares the existing WorldScript rather than
// getting its own. In order:
//   1. Friend poll -- character_social is queried directly rather than the
//      live SocialMgr API, so an offline friending player is still counted
//      correctly (a live Player*-only poll would incorrectly see nothing
//      and unpin a still-friended bot the moment its friend logs off).
//      Newly-friended, not-yet-promoted rows are promoted on the spot
//      (§4.12's ring table: ring 3 is earned "by interaction score crosses
//      threshold, OR friended") and pinned_by_friend is set; rows no longer
//      found friended have pinned_by_friend cleared (§4.12: "unfriending is
//      not observable via hooks... revalidated by polling, not by an
//      event"). Scoped to bots that already have an hside_identity row --
//      a bot friended with zero prior qualifying interaction has no row to
//      find yet, a named gap rather than a cross-database "is this guid a
//      bot" lookup this step doesn't need otherwise.
//   2. Score decay -- one point per day, once a row has gone quiet for
//      kHsScoreDecayGraceDays, floored at 0.
//   3. Card demotion -- dormant (kHsCardDormancyDays quiet), unpinned cards
//      clear card_active and release the exclude-vector entry. Card text is
//      untouched (§4.12: "the card text is not touched").
//   4. Retirement -- currently-online carded bots whose level has dropped
//      below their stored last_known_level are retired via Hs_RetireCard.
void Hs_RunIdentityDailySweep();

// Read-only counters for `.hearthside status`.
uint32_t Hs_PromotionsThisSession();
uint32_t Hs_DemotionsThisSession();
uint32_t Hs_RetirementsThisSession();
uint32_t Hs_IdentityRowCount();     // total hside_identity rows (ring 1+ -- has ever scored)
uint32_t Hs_CardActiveCount();      // ring 3 -- card_active = 1

// §4.19/§7 step 19 -- the GM-tooling and control-API half. Force actions
// bypass the normal earn-it path (score threshold, dormancy timer, level-
// drop detection) for an operator who wants to act on one named bot right
// now, same underlying mechanics as the automatic paths (Hs_BumpInteraction
// Score's promotion, Hs_RunIdentityDailySweep's demotion, Hs_RetireCard's
// retirement) reused rather than duplicated.

// Forces promotion (sets promoted_at if not already set), lazily creating
// the row exactly like Hs_BumpInteractionScore does. Does not touch score --
// the generator's existing "promoted_at IS NOT NULL AND card_voice IS NULL"
// pickup handles card generation the same way it does for a normally-earned
// promotion, so this doesn't jump the queue by calling the LLM synchronously
// from a GM command (which would block the world thread on a network call).
// Returns false if the bot was already promoted.
bool Hs_ForcePromote(uint64_t botGuid, uint8_t botLevel);

// Forces demotion (card_active = 0, exclude-vector entry released) on
// demand -- the same action Hs_RunIdentityDailySweep's dormancy check takes
// automatically, just not waiting for kHsCardDormancyDays. Card text is
// retained (§4.12: demotion is a flag, not a deletion). Returns false if the
// bot had no active card to demote.
bool Hs_ForceDemote(uint64_t botGuid);

// One query's worth of what `.hearthside inspect`/the HTTP inspect route
// need. Ring 3 (card_active) is reported directly since it's bot-global;
// rings 1/2 are only meaningful relative to one specific player (§4.12: ring
// 2 is "has memory rows... with this player"), so rather than force-fit a
// single bot-global "ring" number this reports the row-level facts an
// operator actually needs and a hasAnyMemoryRows flag as a rough signal, not
// a definitive ring.
struct HsIdentityInspection
{
    bool        hasIdentityRow = false;
    std::string archetype;
    uint8_t     lastKnownLevel = 0;
    uint32_t    interactionScore = 0;
    bool        promoted = false;
    bool        cardActive = false;
    bool        pinnedByFriend = false;
    std::string voiceBlock;      // only populated when cardActive
    bool        hasAnyMemoryRows = false;
};
HsIdentityInspection Hs_InspectIdentity(uint64_t botGuid);

// §4.19/§7 step 19: "pin a bot into the exclude list regardless of
// card_active." A raw exclude-vector push/release, independent of the
// card/pinned_by_friend machinery entirely -- an operator can protect any
// named bot from mod-playerbots' recycler (or release one) without that bot
// needing an hside_identity row at all. Thin wrappers over the existing
// Hs_PushBotIntoExcludeVectors/Hs_RemoveBotFromExcludeVectors (step
// 15/17) -- named separately here since GM-initiated pinning is a distinct
// concept from a friend-earned card's decay exemption, even though both
// ultimately touch the same two vectors.
void Hs_GmPinBot(uint64_t botGuid);
void Hs_GmUnpinBot(uint64_t botGuid);

#endif // MOD_HS_IDENTITY_STORE_H
