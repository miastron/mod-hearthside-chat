#ifndef MOD_HS_CHANNEL_H
#define MOD_HS_CHANNEL_H

#include "hs_tier.h"

#include <cstdint>
#include <string>
#include <vector>

// §4.17's global-channel chat surface: policy lookup, WTS/WTB detection (the
// trigger for hs_style.h's Trade `care` offset), and candidate-set ordering
// for the seven channels mod-playerbots joins every bot to unconditionally
// (Trade, General, World, LookingForGroup, GuildRecruitment, LocalDefense,
// WorldDefense -- trap 20).
//
// Pure logic, no AzerothCore dependency -- split like hs_topic_gate.h/
// hs_archetype.h so it's standalone-testable. hs_handler.cpp's Channel*
// overload of OnPlayerCanUseChat is the AC-dependent caller: it resolves a
// live Channel* to an HsChannelKind (via Channel::GetChannelId()/GetName(),
// matched against mod-playerbots' own ChatChannelId enum), reads live
// Player* state into HsChannelCandidate, and calls into this module.
enum class HsChannelKind
{
    Trade,
    General,
    World,
    LookingForGroup,
    GuildRecruitment,
    LocalDefense,
    WorldDefense,
};

constexpr size_t kHsChannelKindCount = 7;

// Matches the ".conf.dist" key segment exactly (HearthsideChat.Channel.<name>.*)
// so config parsing, logging, and this lookup all agree on one spelling.
const char* Hs_ChannelKindName(HsChannelKind kind);

struct HsChannelPolicy
{
    HsTier   maxTier       = HsTier::Off;
    uint32_t ratePerMin    = 0;
    uint32_t maxCandidates = 0;
};

// Replaces the whole in-memory per-channel policy table. Called once at
// startup (and again on `.reload config`) by hs_config.cpp's
// LoadHearthsideChatConfig, after parsing the 21 HearthsideChat.Channel.*
// keys -- same split as Hs_SetArchetypeTable (hs_archetype.h).
void Hs_SetChannelPolicyTable(const HsChannelPolicy (&table)[kHsChannelKindCount]);

HsChannelPolicy Hs_ChannelPolicyFor(HsChannelKind kind);

// Case-insensitive WTS/WTB/WTT word match (not a substring match -- a name
// or item link containing "wts" mid-word doesn't count). This is the only
// signal the Trade `care` offset keys off (hs_style.h) -- deliberately
// narrower than "any Trade channel activity", per operator preference.
bool Hs_IsWtsWtb(const std::string& text);

struct HsChannelCandidate
{
    uint64_t guid   = 0;
    uint32_t zoneId = 0;
};

// Orders `candidates` zone-local-first (matching `speakerZoneId`), then a
// deterministic pseudo-random fill of the remainder, truncated to
// `maxCandidates`. Deterministic given `rngSeed` so a test can assert exact
// output; the caller varies the seed per triggering message (e.g. from the
// speaking player's GUID) so results aren't identical message to message.
// This is the "sampled, never enumerated" half of §4.17's design (trap 21)
// -- candidates beyond the cap never reach Hs_ArbitrateReplies at all.
std::vector<HsChannelCandidate> Hs_OrderChannelCandidates(
    std::vector<HsChannelCandidate> candidates,
    uint32_t speakerZoneId,
    uint32_t maxCandidates,
    uint64_t rngSeed);

#endif // MOD_HS_CHANNEL_H
