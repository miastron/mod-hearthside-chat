#include "hs_bridge.h"
#include "hs_archetype.h"
#include "hs_config.h"
#include "hs_identity_store.h"
#include "hs_memory_store.h"

#include "Chat.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "SharedDefines.h"
#include "WorldPacket.h"

#include <cctype>
#include <chrono>
#include <iterator>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    char const* const kAddonEnvelope = "HSI\t";
    char const kFieldSeparator = '~';

    std::size_t constexpr kMaxWireLength    = 255;
    std::size_t constexpr kMaxTokenLength   = 40;
    std::size_t constexpr kMaxBotNameLength = 48;
    // The LLM-generated voice block validates up to 400 raw bytes
    // (Hs_ValidateVoiceBlock), well past what fits in one 255-byte addon
    // chat packet alongside its envelope/opcode/token overhead: rather
    // than a second framing layer for what is a cosmetic flavor line, this
    // just truncates. `.hearthside inspect` and the HTTP route already show
    // the untruncated text for anyone who needs it in full.
    std::size_t constexpr kMaxVoiceRawLength  = 170;
    std::size_t constexpr kMaxMemoryRawLength = 150;
    std::size_t constexpr kMaxMemoryLines     = 2;
    std::chrono::milliseconds constexpr kRateLimitWindow(500);

    bool IsBot(Player* p)
    {
        if (!p)
            return false;
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(p);
        return ai && ai->IsBotAI();
    }

    bool HasControlCharacter(std::string const& value)
    {
        for (unsigned char c : value)
            if (c < 0x20 || c == 0x7F)
                return true;
        return false;
    }

    bool IsValidToken(std::string const& token)
    {
        if (token.empty() || token.size() > kMaxTokenLength)
            return false;
        for (unsigned char c : token)
            if (!std::isalnum(c) && c != '-' && c != '_' && c != '.' && c != ':')
                return false;
        return true;
    }

    std::string UrlDecodeField(std::string const& value)
    {
        std::string out;
        out.reserve(value.size());
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            char c = value[i];
            if (c == '%' && i + 2 < value.size()
                && std::isxdigit(static_cast<unsigned char>(value[i + 1]))
                && std::isxdigit(static_cast<unsigned char>(value[i + 2])))
            {
                out.push_back(static_cast<char>(std::stoi(value.substr(i + 1, 2), nullptr, 16)));
                i += 2;
            }
            else
                out.push_back(c);
        }
        return out;
    }

    std::string UrlEncodeField(std::string const& value)
    {
        std::ostringstream out;
        char const* const hex = "0123456789ABCDEF";
        for (unsigned char c : value)
        {
            if (c == '%' || c == '~' || c == '\r' || c == '\n')
                out << '%' << hex[(c >> 4) & 0x0F] << hex[c & 0x0F];
            else
                out << static_cast<char>(c);
        }
        return out.str();
    }

    std::string TruncateForWire(std::string const& value, std::size_t maxLength)
    {
        if (value.size() <= maxLength)
            return value;
        return value.substr(0, maxLength) + "...";
    }

    std::pair<std::string, std::string> SplitOnce(std::string const& value, char separator)
    {
        std::size_t pos = value.find(separator);
        if (pos == std::string::npos)
            return { value, "" };
        return { value.substr(0, pos), value.substr(pos + 1) };
    }

    std::vector<std::string> SplitFields(std::string const& value)
    {
        std::vector<std::string> fields;
        std::size_t start = 0;
        while (true)
        {
            std::size_t pos = value.find(kFieldSeparator, start);
            if (pos == std::string::npos)
            {
                fields.push_back(value.substr(start));
                break;
            }
            fields.push_back(value.substr(start, pos - start));
            start = pos + 1;
        }
        return fields;
    }

    void SendHsiPacket(Player* player, std::string const& opcode, std::string const& payload)
    {
        if (!player || !player->GetSession())
            return;

        std::string wire = std::string(kAddonEnvelope) + opcode;
        if (!payload.empty())
            wire += std::string(1, kFieldSeparator) + payload;

        if (wire.size() > kMaxWireLength)
        {
            LOG_WARN("modules", "[HearthsideChat] HSI bridge TX dropped, wireBytes={} exceeds {}", wire.size(), kMaxWireLength);
            return;
        }

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, nullptr, wire.c_str());
        player->SendDirectMessage(&data);
    }

    void SendInspectStatus(Player* player, std::string const& token, std::string const& status)
    {
        SendHsiPacket(player, "INSPECT", token + kFieldSeparator + status);
    }

    // Requester-keyed cooldown so a crafted or looping client can't hammer
    // the identity/memory queries below. AzerothCore serializes packet
    // handling for a given WorldSession on the world update thread, the
    // same no-mutex assumption mod-multibot-bridge's own rate limiting
    // relies on.
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> g_lastRequestAt;

    // Nothing else erases from that map: not logout, not a sweep: so
    // without this it retains one entry per character that ever sent an
    // inspect, for the life of the worldserver process. An entry older than
    // the window already fails the test below, so dropping it gives the same
    // answer as finding it; prune opportunistically on the call that is
    // walking the map anyway, once it is big enough to be worth the pass.
    std::size_t constexpr kPruneWhenLargerThan = 256;

    bool IsRateLimited(uint64_t requesterGuid)
    {
        auto now = std::chrono::steady_clock::now();
        if (g_lastRequestAt.size() > kPruneWhenLargerThan)
        {
            for (auto it = g_lastRequestAt.begin(); it != g_lastRequestAt.end(); )
                it = (now - it->second >= kRateLimitWindow) ? g_lastRequestAt.erase(it) : std::next(it);
        }

        auto it  = g_lastRequestAt.find(requesterGuid);
        if (it != g_lastRequestAt.end() && now - it->second < kRateLimitWindow)
            return true;
        g_lastRequestAt[requesterGuid] = now;
        return false;
    }

    void HandleGetInspect(Player* player, std::string const& payload)
    {
        std::vector<std::string> fields = SplitFields(payload);
        if (fields.size() != 2)
            return; // malformed: no reliably-extractable token to echo back

        std::string const& encodedName = fields[0];
        std::string const& token       = fields[1];
        if (!IsValidToken(token))
            return;

        if (!player || !player->IsInWorld())
            return;

        if (IsRateLimited(player->GetGUID().GetRawValue()))
        {
            SendInspectStatus(player, token, "RATE_LIMITED");
            return;
        }

        std::string botName = UrlDecodeField(encodedName);
        if (botName.empty() || botName.size() > kMaxBotNameLength)
        {
            SendInspectStatus(player, token, "BAD_REQUEST");
            return;
        }

        Player* bot = ObjectAccessor::FindPlayerByName(botName);
        if (!bot || !bot->IsInWorld())
        {
            SendInspectStatus(player, token, "NOT_FOUND");
            return;
        }

        if (!IsBot(bot))
        {
            SendInspectStatus(player, token, "NOT_A_BOT");
            return;
        }

        uint64_t const botGuid = bot->GetGUID().GetRawValue();

        // Archetype is a pure GUID+level draw (hs_archetype.h), available
        // for every bot whether or not it has ever earned an identity row
        //: unlike the voice/personality card below, which only exists
        // once promoted. This is deliberately not insp.archetype (below):
        // that field is only populated once hasIdentityRow is true, and
        // would leave freshly-met bots showing nothing at all.
        HsArchetype const       archetype = Hs_ArchetypeForBot(botGuid, bot->GetLevel());
        HsArchetypeInfo const   info      = Hs_ArchetypeInfoFor(archetype);
        HsIdentityInspection    insp      = Hs_InspectIdentity(botGuid);

        SendHsiPacket(player, "INSPECT",
            token + "~OK~" + info.enumName + "~" + (insp.cardActive ? "1" : "0"));

        if (insp.cardActive && !insp.voiceBlock.empty())
        {
            std::string voice = TruncateForWire(insp.voiceBlock, kMaxVoiceRawLength);
            SendHsiPacket(player, "INSPECT_VOICE", token + "~" + UrlEncodeField(voice));
        }

        // hasAnyMemoryRows is bot-global (any player this bot ever shared
        // history with), so it's only a cheap pre-filter here: the actual
        // lines below are still looked up scoped to this specific
        // requester, same pair-scoping hs_grounded.h's recall kinds use.
        if (insp.hasAnyMemoryRows)
        {
            uint64_t const requesterGuid = player->GetGUID().GetRawValue();
            std::vector<std::string> memoryLines;

            HsMemoryFact dungeonFact = Hs_LookupLastDungeonRun(botGuid, requesterGuid);
            if (dungeonFact.hasFact)
                memoryLines.push_back(dungeonFact.text);
            else if (Hs_HasGroupedBefore(botGuid, requesterGuid))
                memoryLines.push_back("You've grouped together before.");

            if (memoryLines.size() < kMaxMemoryLines && Hs_HasMetBefore(botGuid, requesterGuid))
                memoryLines.push_back("You've met before.");

            for (std::size_t i = 0; i < memoryLines.size() && i < kMaxMemoryLines; ++i)
            {
                std::string text = TruncateForWire(memoryLines[i], kMaxMemoryRawLength);
                SendHsiPacket(player, "INSPECT_MEMORY",
                    token + "~" + std::to_string(i + 1) + "~" + UrlEncodeField(text));
            }
        }
    }
}

bool HsBridgePlayerScript::OnPlayerCanUseChat(Player* player, uint32_t /*type*/, uint32_t lang, std::string& msg, Player* receiver)
{
    if (!g_HsBridgeEnable || lang != LANG_ADDON || !player || !receiver || player != receiver)
        return true;

    std::size_t const envelopeLength = std::char_traits<char>::length(kAddonEnvelope);
    if (msg.size() < envelopeLength || msg.compare(0, envelopeLength, kAddonEnvelope) != 0)
        return true; // not ours: let it (and every other addon's prefix) through untouched

    std::string const body = msg.substr(envelopeLength);

    // Scanned over the body only, never the whole message: the envelope's own
    // separator is a tab (0x09), which HasControlCharacter would reject --
    // that is what this check is guarding the name/token fields against, not
    // the framing the client is required to send.
    if (msg.size() > kMaxWireLength || HasControlCharacter(body))
        return false; // ours, but malformed: consume it, answer nothing

    auto const [opcode, afterOpcode] = SplitOnce(body, kFieldSeparator);
    if (opcode != "GET")
        return false;

    auto const [requestType, requestPayload] = SplitOnce(afterOpcode, kFieldSeparator);
    if (requestType != "INSPECT")
        return false;

    HandleGetInspect(player, requestPayload);
    return false; // consumed: never let a self-whisper addon message become a real chat send
}
