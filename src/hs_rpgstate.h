#ifndef MOD_HS_RPGSTATE_H
#define MOD_HS_RPGSTATE_H

#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h" // NewRpgStatus
#include "PlayerbotMgr.h"

// Is this bot settled enough to start a conversation nobody asked for?
//
// mod-playerbots keeps a live activity state on every bot
// (`botAI->rpgInfo.GetStatus()`), which this module already reads at four
// call sites to *describe* what a bot is doing when it answers a player
// (hs_queue.cpp's RpgStatusHint). This is the other use of the same fact:
// deciding whether a bot should open its mouth at all. A bot that stops
// mid-run to a quest objective to muse about the scenery is the single
// clearest tell that a "player" is not one.
//
// Two facts, AND-ed, because neither alone is the question:
//
//   - **State** is the stable, coarse one: which phase of its life the bot
//     is in, lasting minutes. It is what separates "on a quest" from
//     "taking a break". On its own it is too coarse -- several states cover
//     both the travel to somewhere and the time spent there.
//   - **isMoving()** is the instantaneous physical one, sampled at scan
//     time. On its own it is far too flighty: a questing bot stops
//     constantly -- looting, casting, at a mailbox, at a vendor, at a fork
//     in the path -- and any of those momentary halts would let it chat
//     mid-task.
//
// Together: the state establishes the bot is in a settled phase, and the
// movement check confirms it is actually stopped rather than still on its
// way. mod-playerbots makes the same call for its own decisions all over
// (CheckMountStateAction, LootAction, ChooseRpgTargetAction), so this is its
// own primitive for "is this bot standing still", not a guess at one.
//
// ---- Why these two states ----
//
// RPG_REST is sitting down for a break; entering it always calls
// SetStandState(UNIT_STAND_STATE_SIT) (NewRpgBaseAction.cpp), so a resting
// bot is reliably stationary and the movement check is merely belt-and-
// braces there.
//
// RPG_WANDER_NPC is a bot that has arrived somewhere -- a town, a camp --
// and is milling about between NPCs, alternately walking to the next one and
// standing still. This is the state the movement check exists for: it picks
// out the standing-still moments, which is exactly the scene worth having
// (two bots stopped near each other, striking up a conversation) and
// excludes the walking ones.
//
// RPG_GO_CAMP is deliberately *not* here despite reading like it should be.
// It is the run *to* a camp, not time spent at one: it carries a target
// position and flips to RPG_WANDER_NPC the moment the bot gets within 10
// yards (NewRpgAction.cpp's NewRpgStatusUpdateAction, a default action at
// relevance 11.0 that runs every non-combat tick). Including it would admit
// exactly the mid-run chatter this gate exists to stop, and AND-ing
// isMoving() onto it would leave it contributing nothing but a dead branch.
// Note hs_queue.cpp's RpgStatusHint describes RPG_GO_CAMP to the model as
// "camping a spot, waiting for something to spawn" -- that string is
// misleading for the same reason, but it only colors a prompt, so it is left
// alone here rather than changed as a drive-by.
//
// Everything else -- RPG_GO_GRIND, RPG_DO_QUEST, RPG_TRAVEL_FLIGHT,
// RPG_OUTDOOR_PVP, RPG_WANDER_RANDOM (milling at a grind spot, i.e. a bot
// that just finished fighting), and RPG_IDLE (a transient that immediately
// rolls into a new activity) -- describes a bot that is on its way somewhere
// or mid-task.
//
// ---- Scope ----
//
// Applies to the three *unprompted /say* producers only (hs_ambient.cpp,
// hs_opener.cpp, hs_script.cpp). It deliberately does not gate direct
// replies, party/raid/guild speech, or the global channels: a bot ignoring a
// player who spoke to it because it happens to be walking somewhere is a
// worse failure than the one this prevents, and Trade/General are typed into
// deliberately from anywhere, so what the typist is doing is irrelevant
// there.
//
// A bot with no PlayerbotAI is not settled rather than settled -- the fact
// is unavailable, and the conservative answer for a producer that speaks on
// its own initiative is silence.
//
// **World thread only.** Touches PlayerbotAI*, so this may never be called
// from the LLM worker thread -- same constraint as every other rpgInfo read
// in the module (hs_handler.cpp's TryDispatch documents why).
inline bool Hs_IsBotSettled(Player* bot)
{
    if (!bot)
        return false;

    // Cheapest of the two and the one that rejects most often on a busy
    // realm, so it goes first.
    if (bot->isMoving())
        return false;

    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (!botAI)
        return false;

    NewRpgStatus status = botAI->rpgInfo.GetStatus();
    return status == RPG_REST || status == RPG_WANDER_NPC;
}

#endif // MOD_HS_RPGSTATE_H
