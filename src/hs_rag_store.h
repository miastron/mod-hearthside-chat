#ifndef MOD_HS_RAG_STORE_H
#define MOD_HS_RAG_STORE_H

// The AzerothCore-facing half of hs_rag.h: loads hside_rag into the
// retriever's in-memory index. Same split, and the same reason for it, as
// hs_grounded_store.h / hs_archetype_store.h -- hs_rag.h stays pure logic so
// Tests/test_hs_rag.cpp can build it against data/rag/*.json with no
// database and no game headers.
//
// hside_rag is generated from those JSON files by data/rag/generate_rag_sql.py,
// so the JSON is the authoring source and the table is what ships. That is
// the one place this differs from the other authored tables in the module,
// where the SQL itself is hand-maintained.
//
// Called from HsRagLifecycleWorldScript (hs_main.cpp) at startup and again on
// `.reload config`, so an operator can re-seed the table and pick it up
// without a restart.
void Hs_LoadRagFromDb();

#endif // MOD_HS_RAG_STORE_H
