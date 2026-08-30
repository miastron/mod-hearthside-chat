#ifndef MOD_HS_EVENT_AFFINITY_STORE_H
#define MOD_HS_EVENT_AFFINITY_STORE_H

// The DB-touching half of the event-affinity table, split from
// hs_event_arbiter.h/.cpp the same way hs_archetype_store.h is split from
// hs_archetype.h: this file owns the hside_event_affinity query and pushes
// the result into the in-memory table via Hs_SetEventAffinityTable.
//
// Loaded at startup and again on `.reload config` (hs_main.cpp's
// HsEventLifecycleWorldScript), so per-event archetype weighting is
// retunable without a rebuild. SQL is the source of truth; the event
// vocabulary and the archetype enum both stay fixed in code.
//
// The table authors only exceptions: any (event_type, archetype) pair
// with no row weighs 1.0. An empty or missing table is therefore not an
// error: every archetype is simply equally likely to react to everything.
// A row naming an unrecognized event_type is skipped and logged rather than
// silently dropped, since the two most likely causes (a typo, or a row
// written for an event this build predates) both want an operator's eyes.
void Hs_LoadEventAffinityFromDb();

#endif // MOD_HS_EVENT_AFFINITY_STORE_H
