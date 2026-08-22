#ifndef MOD_HS_ARCHETYPE_STORE_H
#define MOD_HS_ARCHETYPE_STORE_H

// The DB-touching half of the archetype table, split from hs_archetype.h/.cpp
// the same way hs_identity_store.h is split from hs_identity.h: this file
// owns the hside_archetype query and pushes the result into hs_archetype.h's
// in-memory table via Hs_SetArchetypeTable.
//
// Loads all fifteen rows from hside_archetype (matched to the fixed enum by
// enum_name, not by row order) into memory once, at startup, so every
// per-request archetype lookup (hs_queue.cpp's WorkerLoop, run once per
// reply) stays a plain array index with no query in the hot path -- sourced
// from SQL instead of a compiled literal so weights/care/reply/cap/
// talksAbout/profanity can be retuned by an operator without a rebuild.
//
// If the table is missing a row (fresh install before the seed SQL runs, or
// an operator deleted one), that entry falls back to a zero-weight
// placeholder -- it will never be drawn -- and an error is logged naming the
// missing enum_name.
void Hs_LoadArchetypesFromDb();

#endif // MOD_HS_ARCHETYPE_STORE_H
