#ifndef MOD_HS_CLASS_H
#define MOD_HS_CLASS_H

#include <cstddef>

// The ten playable WotLK class names, lowercase, in one place.
//
// Two vocabularies had to agree and neither derived from the other (review
// item 19): hs_corpus.cpp's Hs_ClassNameFor produces these strings for prompt
// labels, %class substitution and the generator's per-class corpus buckets,
// while hs_identity.cpp validates a generated card's alt-class fact against
// the same set. A spelling change in one -- "death knight" to "deathknight",
// say -- would silently start rejecting every card naming that class, with
// nothing to catch it.
//
// Dependency-free on purpose: hs_identity.cpp's standalone Tests/ harness
// must not pull in AzerothCore. That is also why the class *ids* are not
// here -- mapping CLASS_WARRIOR and friends stays in hs_corpus.cpp, where
// the core's own enum is in scope; this header owns only the words.
namespace HsClass
{
    enum Index
    {
        Warrior, Paladin, Hunter, Rogue, Priest,
        DeathKnight, Shaman, Mage, Warlock, Druid,
        Count
    };

    inline constexpr char const* kNames[Count] = {
        "warrior", "paladin", "hunter", "rogue", "priest",
        "death knight", "shaman", "mage", "warlock", "druid",
    };
}

#endif // MOD_HS_CLASS_H
