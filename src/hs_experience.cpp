#include "hs_experience.h"

#include "hs_prune.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>

// The buffer and the renderer. No AzerothCore headers here by design (see
// hs_experience.h): the hooks that feed this live in hs_experience_store.cpp,
// and everything below is exercised by Tests/test_hs_experience.cpp.

namespace
{
    using Clock = HsPrune::Clock;

    struct HsExperienceRing
    {
        std::deque<HsExperienceEntry> entries;   // oldest first
        Clock::time_point             touchedAt{};
    };

    std::mutex                                        g_ExperienceMutex;
    std::unordered_map<uint64_t, HsExperienceRing>    g_Experience;
    std::atomic<uint32_t>                             g_RecordedThisSession{0};

    // Test-only clock offset. Not atomic-guarded against the map's mutex on
    // purpose: it is written only by the harness, single-threaded, before
    // any read.
    std::atomic<int64_t> g_ClockOffsetSeconds{0};

    // The verb each kind contributes. Past tense and first person throughout,
    // so the rendered block reads as one continuous statement about the bot
    // rather than a table of event records.
    std::string Phrase(const HsExperienceEntry& entry)
    {
        switch (entry.kind)
        {
            case HsExperienceKind::QuestCompleted: return "finished the quest \"" + entry.subject + "\"";
            case HsExperienceKind::QuestAbandoned: return "gave up on the quest \"" + entry.subject + "\"";
            case HsExperienceKind::LootedItem:     return "picked up " + entry.subject;
            case HsExperienceKind::SkillUp:        return "reached " + entry.subject;
            case HsExperienceKind::LevelUp:        return "hit level " + entry.subject;
            case HsExperienceKind::MoneyGained:    return "made about " + entry.subject + " gold";
            case HsExperienceKind::ZoneEntered:    return "arrived in " + entry.subject;
            case HsExperienceKind::Died:           return "died in " + entry.subject;
            case HsExperienceKind::DuelWon:        return "won a duel against " + entry.subject;
            case HsExperienceKind::DuelLost:       return "lost a duel to " + entry.subject;
            // Phrased as the join rather than as "are in a party" because
            // every other clause in this block is something the bot did and
            // the prefix reads "you ..." -- a state clause mid-list would not
            // scan. The subject ("a party") is fixed, so it is spelled out
            // here rather than interpolated.
            case HsExperienceKind::PartyJoined:    return "joined a party";
        }
        return "";
    }
}

const char* Hs_ExperienceKindName(HsExperienceKind kind)
{
    switch (kind)
    {
        case HsExperienceKind::QuestCompleted: return "quest_completed";
        case HsExperienceKind::QuestAbandoned: return "quest_abandoned";
        case HsExperienceKind::LootedItem:     return "looted_item";
        case HsExperienceKind::SkillUp:        return "skill_up";
        case HsExperienceKind::LevelUp:        return "level_up";
        case HsExperienceKind::MoneyGained:    return "money_gained";
        case HsExperienceKind::ZoneEntered:    return "zone_entered";
        case HsExperienceKind::Died:           return "died";
        case HsExperienceKind::DuelWon:        return "duel_won";
        case HsExperienceKind::DuelLost:       return "duel_lost";
        case HsExperienceKind::PartyJoined:    return "party_joined";
    }
    return "unknown";
}

int64_t Hs_ExperienceNowSeconds()
{
    auto raw = std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count();
    return static_cast<int64_t>(raw) + g_ClockOffsetSeconds.load();
}

void Hs_RecordExperience(uint64_t botGuid, HsExperienceKind kind, const std::string& subject)
{
    if (subject.empty())
        return;

    std::lock_guard<std::mutex> lock(g_ExperienceMutex);

    int64_t           nowSec = Hs_ExperienceNowSeconds();
    Clock::time_point now    = Clock::now();

    HsExperienceRing& ring = g_Experience[botGuid];
    ring.touchedAt = now;

    // Collapse rather than stack. This is what keeps the noisy hooks
    // (loot, money, skill ticks, zone borders) from flooding a six-slot
    // ring, and it is also where the aggregate comes from: four deaths in
    // one zone become one entry with count 4.
    auto existing = std::find_if(ring.entries.begin(), ring.entries.end(),
        [&](const HsExperienceEntry& e) { return e.kind == kind && e.subject == subject; });

    if (existing != ring.entries.end())
    {
        existing->count += 1;
        existing->lastAtSeconds = nowSec;

        // Move to the back so recency ordering stays a property of position
        // and the eviction below never drops the thing that just happened.
        HsExperienceEntry bumped = *existing;
        ring.entries.erase(existing);
        ring.entries.push_back(bumped);
    }
    else
    {
        HsExperienceEntry entry;
        entry.kind          = kind;
        entry.subject       = subject;
        entry.count         = 1;
        entry.lastAtSeconds = nowSec;
        ring.entries.push_back(entry);

        while (ring.entries.size() > kHsExperienceCapPerBot)
            ring.entries.pop_front();
    }

    g_RecordedThisSession.fetch_add(1);

    HsPrune::PruneStale(g_Experience, now, kHsExperienceStaleSeconds, kHsExperiencePruneThreshold,
                        [](const HsExperienceRing& r) { return r.touchedAt; });
}

std::string Hs_ExperienceLine(const std::vector<HsExperienceEntry>& entries, int64_t now,
                              uint32_t maxEntries, uint32_t maxChars, uint32_t windowSeconds)
{
    if (entries.empty() || maxEntries == 0 || maxChars == 0)
        return "";

    // See the header: the framing carries as much weight as the contents.
    // A bare list invites a small model to read it back.
    const std::string prefix =
        "Background on what you have been up to lately. Don't bring it up yourself, but it's "
        "true if someone asks: you ";

    std::string line;
    uint32_t    used = 0;

    for (const HsExperienceEntry& entry : entries)
    {
        if (used >= maxEntries)
            break;
        if (windowSeconds != 0 && now - entry.lastAtSeconds > static_cast<int64_t>(windowSeconds))
            continue;

        std::string piece = Phrase(entry);
        if (piece.empty())
            continue;
        if (entry.count > 1)
            piece += " (" + std::to_string(entry.count) + " times)";
        if (used > 0)
            piece = "; " + piece;

        // Whole entries only. Unlike a RAG paragraph, each piece here is one
        // short clause, so a severed one reads as a mistake rather than as
        // an abbreviation -- there is nothing to gain by cutting mid-phrase.
        if (prefix.size() + line.size() + piece.size() + 1 > maxChars)
            break;

        line += piece;
        ++used;
    }

    if (line.empty())
        return "";

    return prefix + line + ".";
}

std::string Hs_ExperienceContext(uint64_t botGuid, uint32_t maxEntries, uint32_t maxChars, uint32_t windowSeconds)
{
    std::vector<HsExperienceEntry> snapshot;
    int64_t                        now = 0;

    {
        std::lock_guard<std::mutex> lock(g_ExperienceMutex);
        now = Hs_ExperienceNowSeconds();

        auto it = g_Experience.find(botGuid);
        if (it == g_Experience.end() || it->second.entries.empty())
            return "";

        snapshot.assign(it->second.entries.rbegin(), it->second.entries.rend());
    }

    return Hs_ExperienceLine(snapshot, now, maxEntries, maxChars, windowSeconds);
}

std::vector<HsExperienceEntry> Hs_ExperienceSnapshot(uint64_t botGuid)
{
    std::lock_guard<std::mutex> lock(g_ExperienceMutex);

    auto it = g_Experience.find(botGuid);
    if (it == g_Experience.end())
        return {};

    return std::vector<HsExperienceEntry>(it->second.entries.rbegin(), it->second.entries.rend());
}

void Hs_ForgetExperience(uint64_t botGuid)
{
    std::lock_guard<std::mutex> lock(g_ExperienceMutex);
    g_Experience.erase(botGuid);
}

uint32_t Hs_ExperienceTrackedBotCount()
{
    std::lock_guard<std::mutex> lock(g_ExperienceMutex);
    return static_cast<uint32_t>(g_Experience.size());
}

uint32_t Hs_ExperienceRecordedThisSession()
{
    return g_RecordedThisSession.load();
}

void Hs_ExperienceAdvanceClockForTest(int64_t deltaSeconds)
{
    g_ClockOffsetSeconds.fetch_add(deltaSeconds);
}

void Hs_ExperienceResetForTest()
{
    std::lock_guard<std::mutex> lock(g_ExperienceMutex);
    g_Experience.clear();
    g_ClockOffsetSeconds.store(0);
    g_RecordedThisSession.store(0);
}
