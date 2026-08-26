#ifndef MOD_HS_AMBIENT_H
#define MOD_HS_AMBIENT_H

#include "ScriptMgr.h"
#include <cstdint>

// Unprompted ambient chatter -- the producer HearthsideChat.MaxTier.Ambient
// has always claimed to gate (Claude/PLAN-AMBIENT.md). Until this file
// existed that key was parsed, live-reloaded, and echoed back by both
// operator status surfaces while nothing read it, so setting it to "off" to
// quiet a realm did nothing while `.hearthside status` reported "off" as
// though it had taken.
//
// Ambient is the one surface in this module with *no trigger at all*. Every
// other one needs something to react to: a player spoke (direct reply,
// reflex, grounded), a shared context arose (openers), a game event happened
// (hs_event.cpp), another bot's line landed (hs_botchain.cpp), or the bot had
// already answered this player once (hs_engagement.cpp). Ambient fires
// because there is dead air. That is the point of it, and also the entire
// design problem -- see "volume" below.
//
// The content it speaks already existed before this producer did. The five
// non-opener, non-channel corpus categories (chat_gripe_general,
// chat_class_banter, chat_levelband_musing, chat_faction_banter,
// chat_zone_musing) are documented in docs/architecture.md as ambient "dead
// air" flavor, and the idle-time generator has been filling them on
// Generator.Enable alone -- but their only consumer was hs_handler.cpp's
// direct-reply corpus fallback. The GPU was writing dead-air lines that could
// only ever surface when a player talked to a bot first.
//
// ---- Volume is the whole design ----
//
// Every other surface is bounded by how often its trigger fires. Players only
// talk so much; mobs only die so often. Ambient is bounded by nothing except
// the clock, which makes it the single easiest way to make a realm feel like
// a bot farm. Three properties follow from that, and none of them is
// incidental:
//
//   1. It spends the *shared* unprompted-speech budget (Hs_AmbientBucketTake,
//      hs_queue.h), not a private one. Openers and scripted scenes already
//      spoke on their own initiative; a third independent producer, each
//      tuned separately to "reasonable", still stacks into constant noise.
//   2. At most one line per scan tick, realm-wide. Not one per surface and
//      not a candidate set -- the arbiter exists to answer "who replies to
//      *this message*", and ambient has no message, so there is nothing to
//      arbitrate. One speaker per tick is also the cheapest possible volume
//      control, and it keeps the failure mode bounded by the tick rate rather
//      than by the bot population.
//   3. Corpus-only, permanently. MaxTier.Ambient is checked as
//      HsTierAllows(ceiling, HsTier::Corpus) exactly as MaxTier.Openers is:
//      unprompted GPU spend against no question is the worst cost-per-value
//      in the module, and the generator already writes this content offline
//      for free. Setting Ambient = inference behaves identically to corpus.
//
// A too-quiet realm is recoverable; a too-noisy one has already spent the
// player's patience (the same asymmetry hs_config.cpp records for the channel
// rate keys). Every default here is therefore a conservative starting guess
// to be tuned upward against live evidence, never downward.
//
// ---- Surfaces ----
//
// /say, Trade, General, party and raid. Battleground is deliberately
// absent: mod-playerbots has no SayToBG and SayToRaid hardcodes
// CHAT_MSG_RAID, so BG would need a new HsReplyChannel, a new delivery path,
// and new content -- and a battleground is the one context where chat is
// overwhelmingly tactical, so corpus-authored musing landing mid-fight may
// read worse than silence. See PLAN-AMBIENT.md §3.
//
// One surface is chosen per tick before any scanning happens, then scanned.
// That ordering is deliberate: resolving a live Channel* is by far the most
// expensive test in a player walk (hs_script.cpp says the same of its own
// channel scan), so this way a tick pays for at most one surface's scan
// rather than all six. Choosing among enabled surfaces rather than among
// surfaces already known to be eligible means some ticks find nothing and do
// nothing, which at a 30-second cadence costs nothing worth optimizing.

class HsAmbientScanWorldScript : public WorldScript
{
public:
    HsAmbientScanWorldScript() : WorldScript("HsAmbientScanWorldScript") {}
    void OnUpdate(uint32_t diff) override;
};

// Read-only status for `.hearthside status` and the HTTP status route --
// the only visibility an unprompted subsystem has when nobody is watching a
// game client. Counts lines actually delivered, not ticks attempted.
uint32_t Hs_AmbientLinesFiredThisSession();

// Spend this bot's ambient cooldown without speaking an ambient line.
//
// For the other unprompted producers: a bot that just spoke on its own
// initiative has said its piece, and ambient must not immediately follow it
// with a second unprompted line. The shared token bucket does not cover this
// -- it bounds realm-wide volume, not one bot talking twice in a row -- and
// neither does the script-run check in BotBaseEligible, which only knows
// about hs_script.cpp's scenes. hs_opener.cpp calls this after every opener
// it delivers, which is what stops the opener/ambient pileup a group-join
// used to produce (an opener, then an ambient_party_downtime line seconds
// later, from the same bot).
void Hs_MarkAmbientSpoke(uint64_t botGuid);

#endif // MOD_HS_AMBIENT_H
