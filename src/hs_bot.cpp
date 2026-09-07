#include "hs_bot.h"

#include "hs_config.h" // Hs_IsExcludedBotName

#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

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
