#ifndef MOD_HS_ARCHETYPE_STORE_H
#define MOD_HS_ARCHETYPE_STORE_H

#include "hs_archetype.h" // HsArchetype

#include <cstdint>

// The DB-touching half of the archetype table, split from hs_archetype.h/.cpp
// the same way hs_identity_store.h is split from hs_identity.h: this file
// owns the hside_archetype query and pushes the result into hs_archetype.h's
// in-memory table via Hs_SetArchetypeTable.
//
// Loads all thirteen rows from hside_archetype (matched to the fixed enum by
// enum_name, not by row order) into memory once, at startup, so every
// per-request archetype lookup (hs_queue.cpp's WorkerLoop, run once per
// reply) stays a plain array index with no query in the hot path. Sourced
// from SQL instead of a compiled literal so weights/care/reply/cap/
// talksAbout/profanity can be retuned by an operator without a rebuild.
//
// If the table is missing a row (fresh install before the seed SQL runs, or
// an operator deleted one), that entry falls back to a zero-weight
// placeholder (it will never be drawn) and an error is logged naming the
// missing enum_name.
void Hs_LoadArchetypesFromDb();

// The GM-override counterpart (hs_command.cpp's `.hearthside archetype`);
// this file owns hside_archetype_override, hs_archetype.h/.cpp own the
// in-memory map. Called once at startup, after Hs_LoadArchetypesFromDb.
// An override naming an enum_name no longer in the loaded archetype table
// is skipped and logged, not silently applied against a missing row.
void Hs_LoadArchetypeOverridesFromDb();

// Writes through to both hside_archetype_override and hs_archetype.h's
// in-memory map, so a GM pin survives a restart without a second load call.
void Hs_SetArchetypeOverrideAndPersist(uint64_t botGuid, HsArchetype archetype);
void Hs_ClearArchetypeOverrideAndPersist(uint64_t botGuid);

#endif // MOD_HS_ARCHETYPE_STORE_H
