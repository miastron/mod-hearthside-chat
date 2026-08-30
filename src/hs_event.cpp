#include "hs_event.h"
#include "hs_archetype.h"
#include "hs_config.h"
#include "hs_event_arbiter.h"
#include "hs_queue.h"
#include "hs_tier.h"
#include "hs_topic_gate.h"

#include "DBCStores.h"
#include "Group.h"
#include "GroupReference.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h" // NewRpgStatus, rpgInfo.GetStatus()
#include "PlayerbotMgr.h"
#include "SharedDefines.h"

#include <atomic>
#include <string>
#include <utility>
#include <vector>

namespace
{
    std::atomic<uint32_t> g_EventsFiredThisSession{ 0 };

    bool IsBot(Player* p)
    {
        if (!p)
            return false;
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(p);
        return ai && ai->IsBotAI();
    }

    // Same read hs_handler.cpp's BuildTopicGateContext does, duplicated for
    // the same reason it is already duplicated in hs_engagement.cpp:
    // hs_topic_gate.h stays pure/no-AzerothCore for standalone testing, so
    // the Player*-reading half has to live at each call site.
    HsTopicGateContext BuildTopicGateContext(Player* bot)
    {
        HsTopicGateContext ctx;
        ctx.avgItemLevel = static_cast<uint32_t>(bot->GetAverageItemLevel());

        if (Group* group = bot->GetGroup())
        {
            ctx.inGroup       = true;
            ctx.isGroupLeader = group->IsLeader(bot->GetGUID());
        }

        if (Map* map = bot->GetMap())
        {
            ctx.inInstance = map->IsDungeon() || map->IsRaid();
            if (ctx.inInstance)
                ctx.instanceName = map->GetMapName();
        }

        ctx.goldCopper = bot->GetMoney();

        if (AreaTableEntry const* entry = sAreaTableStore.LookupEntry(bot->GetZoneId()))
        {
            const char* name = entry->area_name[0];
            if (name && *name)
                ctx.zoneName = name;
        }

        return ctx;
    }

    // ---- Why no trigger below names a zone ---------------------------------
    // Claude/archive/PLAN-ARBITER.md §6 originally called for a bracketed "[Elwynn Forest] ..."
    // state line on every non-death trigger. Dropped 2026-08-24 (operator
    // decision) for three reasons, in order of weight:
    //
    // 1. It is already in the prompt. BuildTopicGateContext above reads the
    //    zone (and the instance name inside a dungeon), and hs_topic_gate.cpp
    //    turns it into "You are currently in Elwynn Forest." on the persona
    //    line for every one of these requests. A bracketed copy states it
    //    twice.
    // 2. It is the one detail no reaction is ever *about*. Nobody reacts to a
    //    ding because of the zone, so the token is pure surface area for the
    //    model to invent around, and a zone name is the highest-risk kind,
    //    since a model with any WoW pretraining can volunteer Goldshire or
    //    Hogger off "Elwynn Forest", none of it stated and some of it wrong
    //    for the situation. This is §7 rule 1's reasoning ("write to the
    //    ignorance"), which the death triggers already applied; it holds
    //    everywhere, not only for deaths.
    // 3. No bracket in the user slot means no bracket syntax for the model to
    //    echo back into a reply, the risk lint_dataset.py's ROLEPLAY guard
    //    was widened to catch. That guard stays as belt-and-braces.
    //
    // Player and item names are deliberately NOT covered by this and stay in
    // the triggers below: the reaction *is* the name ("stay down, grimtusk",
    // "gz faeltha"), stripping the addressee would force the model to guess
    // one, and unlike a zone they are runtime-substituted proper nouns it has
    // no priors about: all it can do is repeat them.

    // One actor the fire site is offering to the arbiter: the bot, how
    // involved it is, which event type its *own* affinity resolves against,
    // and the trigger text to send if it is the one selected. Keeping the
    // trigger per-actor is what lets a duel end arbitrate once over
    // {winner, loser} and still say the right thing to whichever side wins
    // the draw (Claude/archive/PLAN-ARBITER.md §2).
    struct HsEventActor
    {
        Player*            bot;
        HsEventInvolvement involvement;
        HsEventType        affinityType;
        std::string        trigger;
    };

    bool EligibleBot(Player* bot)
    {
        if (!bot || !IsBot(bot) || !bot->IsInWorld())
            return false;
        // HearthsideChat.ExcludeNames: never spoken through, no tier at
        // all, same rule every chat surface applies.
        if (Hs_IsExcludedBotName(bot->GetName()))
            return false;
        // Deliberately no g_HsDisableRepliesInCombat check here: see the
        // combat-gate note in hs_event.h.
        return true;
    }

    // The one place every trigger converges: ceiling, event budget,
    // candidate assembly, arbitration, dispatch.
    //
    // `origin` is whoever the event happened around: the player who died,
    // the one who dinged, the duel opponent. It becomes the request's
    // "sender", which is what the style pass protects from typo injection
    // and what proximity is measured from. Nothing is scored against it:
    // Hs_TryEnqueue's isEvent flag suppresses the history append, the
    // interaction-score bump, and the first-meeting record, so an origin
    // that is itself a bot can never seed identity state.
    void FireEvent(HsEventType primaryType, Player* origin, std::vector<HsEventActor>& actors, HsReplyChannel channel)
    {
        if (!g_HsEnable || !origin || !origin->IsInWorld() || actors.empty())
            return;

        // A ceiling is permission, not budget. Events have no corpus
        // fallback: a canned line reacting to a specific death or roll
        // would have to be generic enough to be wrong most of the time, so
        // anything below inference is silence, not a downgrade.
        HsTier ceiling = HsParseTier(g_HsMaxTierEvents);
        if (!HsTierAllows(ceiling, HsTier::Inference))
            return;

        // Spent once per event, before any per-candidate work, so a busy
        // dungeon's stream of deaths and rolls costs almost nothing when the
        // budget is already gone. This bucket is deliberately separate from
        // the tier-2 reply bucket: unbudgeted ambient reactions would
        // otherwise starve replies to players who actually spoke, which is
        // the thing players notice (Claude/archive/PLAN-ARBITER.md §8).
        if (!Hs_EventBucketTake())
            return;

        std::vector<HsEventCandidate> candidates;
        std::vector<Player*>          bots;
        candidates.reserve(actors.size());
        bots.reserve(actors.size());

        for (auto const& actor : actors)
        {
            if (!EligibleBot(actor.bot))
                continue;

            uint64_t botGuid = actor.bot->GetGUID().GetRawValue();

            HsEventCandidate candidate;
            candidate.botGuid               = botGuid;
            candidate.sameMap               = (actor.bot->GetMapId() == origin->GetMapId());
            candidate.distance              = candidate.sameMap ? actor.bot->GetDistance(origin) : 0.0f;
            candidate.secondsSinceLastReply = Hs_SecondsSinceLastReply(botGuid);
            candidate.involvement           = actor.involvement;
            candidate.trigger               = actor.trigger;

            HsArchetype archetype = Hs_ArchetypeForBot(botGuid, actor.bot->GetLevel());
            candidate.affinityWeight = Hs_EventAffinityWeight(actor.affinityType,
                Hs_ArchetypeInfoFor(archetype).enumName);

            candidates.push_back(std::move(candidate));
            bots.push_back(actor.bot);
        }

        if (candidates.empty())
            return;

        std::vector<size_t> selected = Hs_ArbitrateEventReplies(
            Hs_EventCountBiasFor(primaryType), g_HsSayDistance, candidates);

        for (size_t index : selected)
        {
            Player* bot = bots[index];

            NewRpgStatus rpgStatus = RPG_IDLE;
            if (PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
                rpgStatus = botAI->rpgInfo.GetStatus();

            HsTopicGateContext topicGate = BuildTopicGateContext(bot);

            bool admitted = Hs_TryEnqueue(candidates[index].botGuid, bot->GetName(),
                origin->GetGUID().GetRawValue(), origin->GetName(), channel,
                candidates[index].trigger, bot->IsInCombat(), bot->GetLevel(), rpgStatus,
                topicGate, /*isFollowUp=*/false, /*isEvent=*/true);

            if (admitted)
                g_EventsFiredThisSession.fetch_add(1);
            else if (g_HsDebugEnabled)
                LOG_INFO("server.loading", "[HearthsideChat] Event {} enqueue rejected for bot {}.",
                    Hs_EventTypeName(primaryType), bot->GetName());
        }
    }

    // Party or raid, chosen by the group's own shape: PlayerbotAI::SayToParty
    // and ::SayToRaid are different calls, so this is a delivery fact, not a
    // cosmetic one (hs_queue.h).
    HsReplyChannel GroupChannelFor(Group* group)
    {
        return (group && group->isRaidGroup()) ? HsReplyChannel::Raid : HsReplyChannel::Party;
    }

    // Bots within say range of the origin, on the same map and phase, same
    // faction. Used only by world-scoped events, which deliberately do not
    // require a real player nearby.
    std::vector<Player*> NearbyBots(Player* origin, Player* exclude)
    {
        std::vector<Player*> found;
        for (auto const& itr : ObjectAccessor::GetPlayers())
        {
            Player* candidate = itr.second;
            if (!candidate || candidate == origin || candidate == exclude)
                continue;
            if (!EligibleBot(candidate))
                continue;
            if (candidate->GetTeamId() != origin->GetTeamId())
                continue; // opposing faction can't read /say
            // IsWithinDistInMap, not GetDistance: GetPlayers() is realm-wide
            // and a bare distance is map- and phase-blind, so two instances
            // of one dungeon would read as the same room (hs_handler.cpp).
            if (!candidate->IsWithinDistInMap(origin, g_HsSayDistance))
                continue;
            found.push_back(candidate);
        }
        return found;
    }
}

void HsEventDeathHandler::OnPlayerJustDied(Player* player)
{
    if (!g_HsEnable || !player || !player->IsInWorld())
        return;

    Group* group = player->GetGroup();
    std::vector<HsEventActor> actors;

    if (group)
    {
        // A wipe is checked first because it supersedes the individual
        // death: if this death left nobody standing, the group has one
        // thing to react to, not one per corpse. Only the death that
        // completes the wipe can see every member dead, so this fires once
        // without needing its own cooldown.
        bool anyoneAlive = false;
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (member && member->IsInWorld() && member != player && member->IsAlive())
            {
                anyoneAlive = true;
                break;
            }
        }

        if (!anyoneAlive)
        {
            // Second person for every member: a wipe happened to all of
            // them, so there is no witnessed-from-outside phrasing to pick.
            // No zone, no killer, no combat-state clause: death triggers
            // stay bare so there is nothing for the model to invent from
            // (Claude/archive/PLAN-ARBITER.md §7 rule 1).
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (!member || !EligibleBot(member))
                    continue;
                actors.push_back({ member,
                    member == player ? HsEventInvolvement::Subject : HsEventInvolvement::Affected,
                    HsEventType::DeathWipe,
                    "Your whole group has just been wiped out." });
            }
            FireEvent(HsEventType::DeathWipe, player, actors, GroupChannelFor(group));
            return;
        }

        if (IsBot(player))
        {
            if (!EligibleBot(player))
                return;
            actors.push_back({ player, HsEventInvolvement::Subject, HsEventType::DeathInGroup,
                "You are in a group and have just been killed." });
            FireEvent(HsEventType::DeathInGroup, player, actors, GroupChannelFor(group));
            return;
        }

        // A real player in the group died. Candidates are the group's bots,
        // who watched it happen: third person, naming only the fact the
        // trigger states.
        std::string trigger = std::string(player->GetName()) + ", in your group, has just been killed.";
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || member == player || !EligibleBot(member))
                continue;
            actors.push_back({ member, HsEventInvolvement::Affected, HsEventType::DeathGroupPlayer, trigger });
        }
        FireEvent(HsEventType::DeathGroupPlayer, player, actors, GroupChannelFor(group));
        return;
    }

    // Ungrouped. Only a bot's own solo death is a trigger: a stranger
    // dying nearby is not one of Claude/archive/PLAN-ARBITER.md §5's sixteen.
    if (!IsBot(player) || !EligibleBot(player))
        return;

    actors.push_back({ player, HsEventInvolvement::Subject, HsEventType::DeathSolo,
        "You were out on your own and have just been killed." });
    FireEvent(HsEventType::DeathSolo, player, actors, HsReplyChannel::Say);
}

void HsEventLevelHandler::OnPlayerLevelChanged(Player* player, uint8 oldlevel)
{
    if (!g_HsEnable || !player || !player->IsInWorld())
        return;

    // The hook fires on any change; a GM down-level is not a ding. There is
    // no way to tell an organic ding from a RandomPlayerbotMgr bracket
    // relevel through this signature, and that noise is accepted
    // (Claude/archive/PLAN-ARBITER.md §3).
    uint8 newLevel = player->GetLevel();
    if (newLevel <= oldlevel)
    {
        // A *drop* is how mod-playerbots' recycler shows up here:
        // RandomBotLevelMgr::ResetBot knocks a bot back down a bracket in
        // place, keeping its GUID and name. Nothing about the character it
        // was survives that, so its conversation history stops being useful
        // prior-turn context and starts being wrong. This hook is the only
        // signal that covers every bot: hside_identity's own retirement
        // sweep only walks carded rows.
        if (newLevel < oldlevel && IsBot(player))
            Hs_ForgetBotHistory(player->GetGUID().GetRawValue());
        return;
    }

    std::string levelText = std::to_string(static_cast<uint32_t>(newLevel));
    std::vector<HsEventActor> actors;

    // The one who dinged speaks in second person, everyone else in third:
    // one arbitration over the combined pool, each side carrying its own
    // trigger and its own affinity type (the same shape the duel end uses).
    if (IsBot(player) && EligibleBot(player))
    {
        actors.push_back({ player, HsEventInvolvement::Subject, HsEventType::LevelUpSelf,
            "You have just reached level " + levelText + "." });
    }

    std::string witnessTrigger = std::string(player->GetName()) +
        " has just reached level " + levelText + ".";

    Group* group = player->GetGroup();
    if (group)
    {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || member == player || !EligibleBot(member))
                continue;
            actors.push_back({ member, HsEventInvolvement::Witness, HsEventType::LevelUpGroup, witnessTrigger });
        }
    }
    else
    {
        // World-scoped: a ding in the open is worth a "gz" from whoever is
        // standing there, real player present or not.
        for (Player* nearby : NearbyBots(player, nullptr))
            actors.push_back({ nearby, HsEventInvolvement::Witness, HsEventType::LevelUpGroup, witnessTrigger });
    }

    // LEVEL_UP_SELF drives the reply count when the bot itself dinged, since
    // that is the more constrained draw; a real player's ding falls back to
    // the witness bias, which is the one that almost always produces a "gz".
    HsEventType primary = (IsBot(player) && EligibleBot(player))
        ? HsEventType::LevelUpSelf : HsEventType::LevelUpGroup;

    FireEvent(primary, player, actors, group ? GroupChannelFor(group) : HsReplyChannel::Say);
}

void HsEventPvpKillHandler::OnPlayerPVPKill(Player* killer, Player* killed)
{
    if (!g_HsEnable || !killer || !killed || !EligibleBot(killer))
        return;

    std::vector<HsEventActor> actors;
    actors.push_back({ killer, HsEventInvolvement::Subject, HsEventType::KillingBlow,
        "You have just killed " + std::string(killed->GetName()) + " in a fight." });

    // Spoken to the killer's own side. The victim is cross-faction and
    // cannot read it either way; the audience is whoever shares the killer's
    // group, or /say range if it has none.
    Group* group = killer->GetGroup();
    FireEvent(HsEventType::KillingBlow, killer, actors,
        group ? GroupChannelFor(group) : HsReplyChannel::Say);
}

void HsEventRollHandler::OnPlayerGroupRollRewardItem(Player* player, Item* item, uint32 /*count*/,
                                                      RollVote /*voteType*/, Roll* roll)
{
    if (!g_HsEnable || !player || !item || !roll)
        return;

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return;
    // A group roll happens on greens too; without this the pair is noise.
    if (proto->Quality < ITEM_QUALITY_RARE)
        return;

    Group* group = player->GetGroup();
    if (!group)
        return;

    std::string itemName = proto->Name1;
    std::vector<HsEventActor> actors;

    if (EligibleBot(player))
    {
        actors.push_back({ player, HsEventInvolvement::Subject, HsEventType::RollWon,
            "You have just won the roll for " + itemName + "." });
    }

    // Only bots that actually rolled on it lost anything. A bot that
    // passed has nothing to react to, and treating it as a candidate is the
    // kind of state the trigger never established (Claude/archive/PLAN-ARBITER.md §7
    // rule 2).
    std::string lostTrigger = std::string(player->GetName()) +
        " has just won the roll for " + itemName + ".";
    for (auto const& vote : roll->playerVote)
    {
        if (vote.first == player->GetGUID())
            continue;
        if (vote.second != NEED && vote.second != GREED)
            continue;

        Player* loser = ObjectAccessor::FindPlayer(vote.first);
        if (!loser || !EligibleBot(loser))
            continue;
        actors.push_back({ loser, HsEventInvolvement::Affected, HsEventType::RollLost, lostTrigger });
    }

    HsEventType primary = EligibleBot(player) ? HsEventType::RollWon : HsEventType::RollLost;
    FireEvent(primary, player, actors, GroupChannelFor(group));
}

void HsEventDuelHandler::OnPlayerDuelStart(Player* player1, Player* player2)
{
    if (!g_HsEnable || !player1 || !player2)
        return;

    std::vector<HsEventActor> actors;
    auto addSide = [&actors](Player* bot, Player* opponent)
    {
        if (!EligibleBot(bot))
            return;
        actors.push_back({ bot, HsEventInvolvement::Subject, HsEventType::DuelStart,
            "A duel between you and " + std::string(opponent->GetName()) + " is starting." });
    };
    addSide(player1, player2);
    addSide(player2, player1);

    // The two participants are the only valid speakers: a bystander
    // commenting on someone else's duel is not one of the sixteen triggers.
    FireEvent(HsEventType::DuelStart, player1, actors, HsReplyChannel::Say);
}

void HsEventDuelHandler::OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type)
{
    if (!g_HsEnable || !winner || !loser)
        return;
    if (type == DUEL_INTERRUPTED)
        return; // nobody won; there is no outcome to react to

    std::vector<HsEventActor> actors;

    // One pass over {winner, loser}, each carrying its own outcome. Which
    // trigger text reaches Hs_CallLLM is therefore decided by which side the
    // arbiter picks, not fixed here, and each side's affinity resolves
    // against its own event type, so an archetype can be eager to gloat and
    // reluctant to admit a loss (Claude/archive/PLAN-ARBITER.md §2).
    if (EligibleBot(winner))
    {
        actors.push_back({ winner, HsEventInvolvement::Subject, HsEventType::DuelWon,
            "You have just won a duel against " + std::string(loser->GetName()) + "." });
    }
    if (EligibleBot(loser))
    {
        actors.push_back({ loser, HsEventInvolvement::Subject, HsEventType::DuelLost,
            "You have just lost a duel to " + std::string(winner->GetName()) + "." });
    }

    // DUEL_WON supplies the count bias for the combined pass; both duel
    // outcomes carry the same heavy bias toward silence, so which one is
    // read here does not change the distribution.
    FireEvent(HsEventType::DuelWon, winner, actors, HsReplyChannel::Say);
}

uint32_t Hs_EventsFiredThisSession()
{
    return g_EventsFiredThisSession.load();
}
