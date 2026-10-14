#include "hs_life.h"
#include "hs_archetype.h"
#include "hs_config.h"
#include "hs_log.h"
#include "hs_script.h"

#include "Log.h"

#if __has_include("../../mod-hearthside-life/src/LifeArchetypes.h")
#define HS_HAS_LIFE 1
#include "../../mod-hearthside-life/src/LifeActivity.h"
#include "../../mod-hearthside-life/src/LifeArchetypes.h"
#include "../../mod-hearthside-life/src/LifeEvents.h"
#include "../../mod-hearthside-life/src/LifeLevels.h"
#endif

bool Hs_LifeActive()
{
#ifdef HS_HAS_LIFE
    return g_HsEnable && g_HsLifeEnable;
#else
    return false;
#endif
}

void Hs_RegisterWithLife()
{
#ifdef HS_HAS_LIFE
    // Hs_ArchetypeForBot and Hs_ArchetypeMaskForNames are exactly what City
    // and PvP called before Life: no bot changes archetype. Both are
    // thread-safe (the table and the overrides are mutex-guarded).
    LifeArchetypes::Provider provider;
    provider.ForBot = [](std::uint64_t guidRaw) { return static_cast<int>(Hs_ArchetypeForBot(guidRaw)); };
    provider.MaskForNames = [](std::string const& csv, std::vector<std::string>& unknown)
        { return Hs_ArchetypeMaskForNames(csv, unknown); };
    provider.Enabled = [] { return Hs_LifeActive(); };
    LifeArchetypes::Register(std::move(provider));

    LifeEvents::Listen([](LifeEvents::Event const& event)
    {
        if (!Hs_LifeActive())
            return;
        switch (event.Type)
        {
            case LifeEvents::Kind::CrowdPair:
                Hs_OfferScriptPair(event.Actor, event.Other, event.Witness);
                break;
            default: // RaidDefenseStarted: no dataset rows yet
                break;
        }
    });
    LOG_INFO(kHsLog, "[HearthsideChat] mod-hearthside-life found: archetype provider and event listener registered.");
#endif
}

void Hs_LifeMarkBusy(uint64_t botGuid, uint32_t durationMs)
{
#ifdef HS_HAS_LIFE
    if (Hs_LifeActive())
        LifeActivity::Mark(botGuid, durationMs);
#else
    (void)botGuid;
    (void)durationMs;
#endif
}

void Hs_LifeClearBusy(uint64_t botGuid)
{
#ifdef HS_HAS_LIFE
    // Cleared even with the bridge off, so a mark set before a reload
    // switched it off does not outlive its scene.
    LifeActivity::Clear(botGuid);
#else
    (void)botGuid;
#endif
}

bool Hs_LifeHoldLevels(std::unordered_set<std::string> const& names)
{
#ifdef HS_HAS_LIFE
    if (!Hs_LifeActive())
        return false;
    LifeBots::HoldLevelsFor(LifeBots::LevelHolderChat, names);
    return true;
#else
    (void)names;
    return false;
#endif
}

void Hs_LifeReleaseLevels()
{
#ifdef HS_HAS_LIFE
    LifeBots::HoldLevelsFor(LifeBots::LevelHolderChat, {});
#endif
}
