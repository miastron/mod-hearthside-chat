#ifndef MOD_HS_AMBIENT_H
#define MOD_HS_AMBIENT_H

#include "ScriptMgr.h"
#include <cstdint>

// Unprompted ambient chatter: a bot musing into dead air on /say, Trade,
// General, party or raid, gated by HearthsideChat.MaxTier.Ambient. It is the
// one surface with no trigger at all -- bounded only by the clock -- so
// volume is the whole design:
//
//   1. It spends the *shared* unprompted-speech budget (Hs_AmbientBucketTake,
//      hs_queue.h) with openers and scripted scenes, so three producers each
//      tuned to "reasonable" can't stack into constant noise.
//   2. At most one line per scan tick, realm-wide. One surface is chosen per
//      tick before any scanning, so a tick pays for at most one scan (a
//      Channel* resolve is the most expensive test in a player walk).
//   3. Corpus-only, permanently: unprompted GPU spend against no question is
//      the worst cost-per-value in the module. Ambient = inference behaves as
//      corpus.
//
// A too-quiet realm is recoverable, a too-noisy one has spent the player's
// patience, so every default is a conservative guess to tune upward.
// Battleground is absent on purpose: mod-playerbots has no SayToBG, and BG
// chat is tactical enough that musing would read worse than silence.

class HsAmbientScanWorldScript : public WorldScript
{
public:
    HsAmbientScanWorldScript() : WorldScript("HsAmbientScanWorldScript") {}
    void OnUpdate(uint32_t diff) override;
};

// Read-only status for `.hearthside status` --
// the only visibility an unprompted subsystem has when nobody is watching a
// game client. Counts lines actually delivered, not ticks attempted.
uint32_t Hs_AmbientLinesFiredThisSession();

// Spend this bot's ambient cooldown without speaking an ambient line.
//
// For the other unprompted producers: a bot that just spoke on its own
// initiative has said its piece, and ambient must not immediately follow it
// with a second unprompted line. The shared token bucket does not cover this
//: it bounds realm-wide volume, not one bot talking twice in a row: and
// neither does the script-run check in BotBaseEligible, which only knows
// about hs_script.cpp's scenes. hs_opener.cpp calls this after every opener
// it delivers, which is what stops the opener/ambient pileup a group-join
// used to produce (an opener, then an ambient_party_downtime line seconds
// later, from the same bot).
void Hs_MarkAmbientSpoke(uint64_t botGuid);

#endif // MOD_HS_AMBIENT_H
