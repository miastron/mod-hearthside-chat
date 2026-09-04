#ifndef MOD_HS_LOCALE_H
#define MOD_HS_LOCALE_H

#include <string>

struct AreaTableEntry;
struct ItemTemplate;
class SpellInfo;

// Review H1 (2026-09-03): one place that knows how to read a localized name.
//
// The module used to index DBC and template name arrays at [0] (enUS) in
// twelve places -- zone names in hs_handler/hs_corpus/hs_event/hs_opener/
// hs_memory_store/hs_engagement/hs_botchain, the mount spell name, and item
// names -- while only channel resolution (hs_queue.cpp) used
// sWorld->GetDefaultDbcLocale(). On a non-enUS realm that meant bots put
// English zone and item names into prompts and corpus placeholders while
// resolving channels by the realm's own locale, so the two disagreed about
// what a channel was even called.
//
// All three follow the same rule, which is the one PlayerbotAI::
// GetLocalizedAreaName already established: prefer the realm's default DBC
// locale, fall back to enUS when that locale has no string. A missing name
// returns "" and every call site treats that the way it already treated a
// null pointer -- silence, or an unresolved placeholder that drops the line,
// never a fabricated substitute.
//
// Not header-only: reading the locale needs sWorld/sObjectMgr, and this
// header is included by files whose standalone test harnesses must stay free
// of AzerothCore dependencies.

// Zone/area display name. Null-safe.
std::string Hs_LocalizedAreaName(AreaTableEntry const* entry);

// Item display name. Null-safe. Falls back to ItemTemplate::Name1 (which is
// the enUS string) when locale_item has no row for this item.
std::string Hs_LocalizedItemName(ItemTemplate const* tmpl);

// Spell display name, for the mount lookup. Null-safe.
std::string Hs_LocalizedSpellName(SpellInfo const* info);

#endif // MOD_HS_LOCALE_H
