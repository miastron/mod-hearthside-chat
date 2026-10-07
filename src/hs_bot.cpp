#include "hs_bot.h"

#include "hs_config.h" // Hs_IsExcludedBotName

#include "CharacterCache.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "WorldSession.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

namespace
{
    // A bot account, whether or not mod-playerbots has attached an AI yet.
    bool IsBotAccount(uint32 accountId, ObjectGuid guid)
    {
        return accountId && (sPlayerbotAIConfig.IsInRandomAccountList(accountId) ||
                             sRandomPlayerbotMgr.IsAddclassBot(guid.GetCounter()));
    }
}

bool Hs_IsBot(Player* p)
{
    if (!p)
        return false;
    if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(p))
        return ai->IsBotAI();
    // No AI attached yet is not the same as human: mod-playerbots attaches it
    // after OnPlayerLogin, so every random bot logging in used to read as a
    // real player there -- and fired a guild-login reaction from its whole
    // guild (2026-10-07 chat log: dozens per minute after a restart).
    return IsBotAccount(p->GetSession() ? p->GetSession()->GetAccountId() : 0, p->GetGUID());
}

bool Hs_IsEligibleBot(Player* p)
{
    return Hs_IsBot(p) && !Hs_IsExcludedBotName(p->GetName());
}

bool Hs_IsBotGuid(uint64_t rawGuid)
{
    if (rawGuid == 0)
        return false;

    ObjectGuid guid(rawGuid);
    if (Player* online = ObjectAccessor::FindPlayer(guid))
        return Hs_IsBot(online);

    return IsBotAccount(sCharacterCache->GetCharacterAccountIdByGuid(guid), guid);
}
