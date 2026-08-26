#ifndef MOD_HS_BOTCHAIN_H
#define MOD_HS_BOTCHAIN_H

#include "hs_channel.h" // HsChannelKind -- itself AzerothCore-free

#include <cstdint>
#include <string>

class Player;

// Forward-declared rather than including hs_queue.h, which pulls
// PlayerbotAIConfig.h and would make this header impossible to compile
// standalone -- the pure-logic half below carries a harness
// (Tests/test_hs_botchain.cpp), the same split hs_channel.h/hs_topic_gate.h
// use. hs_queue.h fixes the underlying type, so this declaration is exact;
// hs_botchain.cpp includes the real definition.
enum class HsReplyChannel : uint8_t;

// Live bot-to-bot chains on party/raid and the zone General channel: one bot's
// delivered line becomes the trigger another bot answers with a real tier-2
// call, rather than a pre-written scene being replayed.
//
// This is the second bot-to-bot mechanism, not a replacement for the first.
// hs_script.cpp replays multi-turn scripts the idle generator wrote earlier;
// that stays exactly as it was. The two are selected by one key:
// MaxTier.BotToBot = "corpus" is scripted replay only, "inference" adds live
// chaining on top of it. A ceiling is permissive, so "inference" never turns
// the scripted path off, and the generator's script reserve
// (hs_generator.cpp) is gated on Generator.Enable alone and keeps filling
// during GPU idle either way.
//
// Why /say is deliberately not a chaining surface: it already has the
// scripted proximity mechanism, and a /say chain has no conversational scope
// to bound it -- every player in range hears it, and there is no group or
// channel membership to key a depth counter on. Party, raid and General all
// have exactly that natural boundary.
//
// The trigger point is delivery (hs_queue.cpp's Hs_DeliverPending), not the
// chat hook. A bot's line goes out through PlayerbotAI::SayToParty /
// Channel::Say and never reaches OnPlayerCanUseChat, so there is nothing for
// a hook-side approach to see -- which is also why every hook in
// hs_handler.cpp can keep its `if (IsBot(player)) return true` guard
// unchanged.

// ---- pure logic (no AzerothCore dependency -- Tests/test_hs_botchain.cpp) --

// A chain's scope is the conversation it belongs to, not a pair of bots:
// everything sharing one scope shares one depth counter, one cooldown, and
// one abort. Scope id 0 is reserved as this module's "not a chain hop"
// sentinel throughout hs_queue.h, so neither constructor may return it.
constexpr uint64_t kHsBotChainChannelScopeBit = 1ull << 63;

inline uint64_t Hs_BotChainScopeForGroup(uint64_t groupGuidRaw)
{
    return groupGuidRaw; // a live group's raw GUID is never 0
}

inline uint64_t Hs_BotChainScopeForChannel(HsChannelKind kind)
{
    // Bit 63 is clear on every ObjectGuid a 3.3.5a group can hold
    // (HIGHGUID_GROUP sits in the 0x1F5x band of the high word), so a channel
    // scope can never collide with a group scope above.
    return kHsBotChainChannelScopeBit | static_cast<uint64_t>(kind);
}

// base * (decay/100)^depth, truncated toward zero -- the same decay shape
// hs_engagement.cpp applies to follow-up chain depth, so the two autonomous
// surfaces taper identically. Depth 0 returns basePercent unchanged.
inline uint32_t Hs_BotChainHopChancePercent(uint32_t basePercent, uint32_t decayPercent, uint32_t depth)
{
    double chance = static_cast<double>(basePercent);
    for (uint32_t i = 0; i < depth; ++i)
        chance *= (static_cast<double>(decayPercent) / 100.0);
    return static_cast<uint32_t>(chance);
}

// ---- runtime (AzerothCore-dependent, hs_botchain.cpp) ---------------------

// Called from Hs_DeliverPending for every successfully delivered line that
// may seed a chain. Applies every gate itself -- tier ceiling, surface,
// depth cap, scope cooldown, decayed chance, the channel's own bucket, and
// the real-player requirement -- so the call site needs no conditions of its
// own, and is a cheap no-op on the surfaces and tiers that never chain.
//
// wasChainHop distinguishes a hop's own line from a line the bot said to a
// real player. A non-hop line resets the scope's depth to 0: it is rooted in
// something a player actually said, so it starts a chain rather than
// continuing one. That is also what makes an interrupted chain resume
// naturally -- see Hs_AbortBotChainsInScope.
void Hs_NoteBotLine(Player* speaker, HsReplyChannel channel, HsChannelKind kind,
                     const std::string& text, bool wasChainHop);

// A real player spoke into this scope: they take the floor. Bumps the
// scope's generation (invalidating any hop still generating, which
// Hs_BotChainHopStillValid then drops at delivery) and resets depth to 0.
//
// Deliberately not "stop chaining here for a while": the bots' replies to
// the player are non-hop lines on this same scope, so they re-seed the chain
// from depth 0 rooted in what the player said. The player joins the
// conversation instead of ending it. Only the stale hop -- the one still
// answering a line from before they spoke -- is thrown away.
//
// Re-seeding is not instant when a chain has just run: a depth-0 seed still
// has to clear BotChain.ScopeCooldownSeconds, so a group that has only just
// finished chatting stays quiet a while longer. Replies *to the player*
// never pass through here at all (hs_handler.cpp answers those directly), so
// nothing about that cooldown makes a bot slower to answer a person.
void Hs_AbortBotChainsInScope(uint64_t scopeId);

// False if the hop's scope was aborted (or forgotten) since it was issued.
// Read by Hs_DeliverPending immediately before speaking a hop's line.
bool Hs_BotChainHopStillValid(uint64_t scopeId, uint32_t chainSeq);

// Read-only status for `.hearthside status`, matching
// Hs_EngagementFollowUpsFiredThisSession's shape.
uint32_t Hs_BotChainHopsFiredThisSession();

#endif // MOD_HS_BOTCHAIN_H
