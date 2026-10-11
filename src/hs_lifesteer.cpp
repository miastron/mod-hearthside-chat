#include "hs_lifesteer.h"

// Compiled only when mod-playerbots-pvp-life sits in the same static modules
// build: AzerothCore's modules/CMakeLists.txt puts every static module's
// source directories on the shared `modules` include path, which is what
// makes these two headers visible here with no .cmake change. Their claim
// registry is a function-local static in an inline function, so it is one
// object across the whole static worldserver binary; under MODULES=dynamic
// each module would get its own copy and steering could not work, and the
// headers are not visible to this module's target anyway, so it compiles out.
#if __has_include("PvPLifeMgr.h") && __has_include("LifeBotReservation.h")

#include "LifeBotReservation.h"
#include "PvPLifeMgr.h"

#include "hs_archetype.h"
#include "hs_bot.h"
#include "hs_config.h"
#include "hs_log.h"

#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // Under PvP Life's minimum arrival stagger (ArrivalStaggerMinSeconds,
    // default 2), so a misfit is usually released before it travels. Cheap:
    // at most ~120 PvP Life claims to hash.
    constexpr uint32 kReleaseScanIntervalMs  = 1000;
    constexpr uint32 kRotationScanIntervalMs = 30000;
    constexpr uint32 kRotationSeconds        = 3600;
    constexpr uint32 kReleaseCooldownSeconds = 600;

    // The registry's only owner slot neither Life module uses for itself.
    // Their status commands will report these claims as "Artisans".
    constexpr PlayerbotsLife::ReservationOwner kHsOwner = PlayerbotsLife::ReservationOwner::Artisans;

    struct HsLifeClaim
    {
        PlayerbotsLife::LeaseToken token;
        uint32 cooldownUntil = 0; // 0 = rotation claim; otherwise a post-release cooldown
    };

    class HsLifeSteerWorldScript : public WorldScript
    {
    public:
        HsLifeSteerWorldScript() : WorldScript("HsLifeSteerWorldScript") {}

        void OnUpdate(uint32 diff) override
        {
            _msSinceRelease  += diff;
            _msSinceRotation += diff;
            if (_msSinceRelease < kReleaseScanIntervalMs)
                return;
            _msSinceRelease = 0;

            if (!g_HsEnable || !RefreshFitMask())
            {
                ReleaseAll();
                return;
            }

            uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());
            ExpireCooldowns(now);
            ReleaseMisfits(now);

            if (_msSinceRotation >= kRotationScanIntervalMs)
            {
                _msSinceRotation = 0;
                ReconcileRotation(now);
            }
        }

        void OnShutdown() override { ReleaseAll(); }

    private:
        // World thread only, like every member here: OnUpdate is the only
        // caller, and g_HsLifePvpArchetypes is written by `.reload config` on
        // that same thread.
        bool RefreshFitMask()
        {
            if (g_HsLifePvpArchetypes != _parsedFrom)
            {
                _parsedFrom = g_HsLifePvpArchetypes;
                _fitMask = 0;
                std::stringstream ss(_parsedFrom);
                std::string token;
                while (std::getline(ss, token, ','))
                {
                    size_t begin = token.find_first_not_of(" \t");
                    if (begin == std::string::npos)
                        continue;
                    token = token.substr(begin, token.find_last_not_of(" \t") - begin + 1);
                    HsArchetype a;
                    if (Hs_ArchetypeForName(token, a))
                        _fitMask |= 1u << static_cast<uint32>(a);
                    else
                        LOG_ERROR(kHsLog, "[HearthsideChat] LifeModules.PvPArchetypes: unknown archetype '{}', ignored.", token);
                }
            }
            return _fitMask != 0;
        }

        bool Fits(uint32 guidLow) const
        {
            uint64 raw = ObjectGuid::Create<HighGuid::Player>(guidLow).GetRawValue();
            return (_fitMask & (1u << static_cast<uint32>(Hs_ArchetypeForBot(raw)))) != 0;
        }

        void Claim(uint32 guidLow, uint32 cooldownUntil)
        {
            HsLifeClaim claim;
            if (PlayerbotsLife::TryAcquireLease(claim.token, guidLow, kHsOwner, 0))
            {
                claim.cooldownUntil = cooldownUntil;
                _claims[guidLow] = claim;
            }
            // A failed acquire means a Life module holds the bot right now;
            // the next pass retries.
        }

        void ReleaseAll()
        {
            for (auto const& [guidLow, claim] : _claims)
                PlayerbotsLife::ReleaseLease(claim.token);
            _claims.clear();
        }

        void ExpireCooldowns(uint32 now)
        {
            for (auto it = _claims.begin(); it != _claims.end();)
            {
                if (it->second.cooldownUntil != 0 && it->second.cooldownUntil <= now)
                {
                    PlayerbotsLife::ReleaseLease(it->second.token);
                    it = _claims.erase(it);
                }
                else
                    ++it;
            }
        }

        void ReleaseMisfits(uint32 now)
        {
            std::vector<uint32> pvpOwned;
            {
                PlayerbotsLife::ReservationRegistry& registry = PlayerbotsLife::GetReservationRegistry();
                std::lock_guard<std::mutex> lock(registry.Mutex);
                for (auto const& [guidLow, owner] : registry.Owners)
                    if (owner == PlayerbotsLife::ReservationOwner::PvPLife)
                        pvpOwned.push_back(guidLow);
            }

            uint32 fitting = 0;
            std::vector<uint32> misfits;
            for (uint32 guidLow : pvpOwned)
            {
                if (Fits(guidLow))
                    ++fitting;
                else
                    misfits.push_back(guidLow);
            }

            uint32 const count = Hs_LifeReleaseCount(fitting, static_cast<uint32>(misfits.size()));
            for (uint32 i = 0; i < count; ++i)
            {
                // Removes the bot from its event, cancels its queued moves and
                // releases PvP Life's claim; a bot it does not recognize as a
                // participant is left untouched (and is recounted next pass).
                sPvPLifeMgr.DisengageParticipant(misfits[i]);
                Claim(misfits[i], now + kReleaseCooldownSeconds);
            }

            if (count && g_HsDebugEnabled)
                LOG_INFO(kHsLog, "[HearthsideChat] Life steering: PvP Life held {} fitting / {} misfit bots, released {}.",
                    fitting, misfits.size(), count);
        }

        void ReconcileRotation(uint32 now)
        {
            uint64 const window = now / kRotationSeconds;
            std::unordered_set<uint32> seen;

            for (auto const& itr : ObjectAccessor::GetPlayers())
            {
                Player* p = itr.second;
                if (!p || !p->IsInWorld() || !Hs_IsBot(p))
                    continue;

                uint32 const guidLow = p->GetGUID().GetCounter();
                seen.insert(guidLow);

                bool const wanted = !Fits(guidLow) &&
                    Hs_IsLifeRotationClaim(p->GetGUID().GetRawValue(), window, g_HsLifeClaimPercent);
                auto it = _claims.find(guidLow);
                if (wanted)
                {
                    if (it == _claims.end())
                        Claim(guidLow, 0);
                    else
                        it->second.cooldownUntil = 0; // a cooldown claim the rotation now wants anyway
                }
                else if (it != _claims.end() && it->second.cooldownUntil == 0)
                {
                    PlayerbotsLife::ReleaseLease(it->second.token);
                    _claims.erase(it);
                }
            }

            // Bots that logged out: neither Life module borrows offline bots
            // by default, so there is nothing to hold them against.
            for (auto it = _claims.begin(); it != _claims.end();)
            {
                if (!seen.count(it->first))
                {
                    PlayerbotsLife::ReleaseLease(it->second.token);
                    it = _claims.erase(it);
                }
                else
                    ++it;
            }

            if (g_HsDebugEnabled)
                LOG_INFO(kHsLog, "[HearthsideChat] Life steering: {} bots online, holding {} claims.",
                    seen.size(), _claims.size());
        }

        uint32 _msSinceRelease  = 0;
        uint32 _msSinceRotation = 0;
        std::string _parsedFrom;
        uint32 _fitMask = 0;
        std::unordered_map<uint32, HsLifeClaim> _claims;
    };
}

void Hs_AddLifeSteerScripts()
{
    new HsLifeSteerWorldScript();
}

#else

void Hs_AddLifeSteerScripts() {}

#endif
