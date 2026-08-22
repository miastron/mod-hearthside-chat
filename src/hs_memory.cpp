#include "hs_memory.h"

std::string Hs_BuildFirstMeetingText()
{
    return "We first crossed paths.";
}

std::string Hs_BuildDungeonCompletedText(const std::string& dungeonOrRaidName)
{
    return "We cleared " + dungeonOrRaidName + " together.";
}

std::string Hs_BuildGroupedInZoneText(const std::string& zoneName)
{
    return "We grouped up in " + zoneName + ".";
}

std::string Hs_BuildDiedTogetherText(const std::string& zoneName)
{
    return "We went down together in " + zoneName + ".";
}

std::string Hs_BuildJoinedSameGuildText(const std::string& guildName)
{
    return "We ended up in " + guildName + " together.";
}
