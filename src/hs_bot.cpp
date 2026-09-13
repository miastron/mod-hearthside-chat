#include "hs_bot.h"

#include "hs_config.h" // Hs_IsExcludedBotName

#include "CharacterCache.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

bool Hs_IsBot(Player* p)
{
    if (!p)
        return false;
    PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(p);
    return ai && ai->IsBotAI();
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

    uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(guid);
    if (!accountId)
        return false;

    return sPlayerbotAIConfig.IsInRandomAccountList(accountId) ||
           sRandomPlayerbotMgr.IsAddclassBot(guid.GetCounter());
}
