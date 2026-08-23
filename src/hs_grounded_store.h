#ifndef MOD_HS_GROUNDED_STORE_H
#define MOD_HS_GROUNDED_STORE_H

// Loads hside_grounded_question/hside_grounded_template into memory via
// hs_grounded.h's Hs_SetGroundedQuestionTable/Hs_SetGroundedTemplateTable.
// AzerothCore-dependent (DatabaseEnv.h) -- split from hs_grounded.h/.cpp the
// same way hs_archetype_store.h/.cpp splits from hs_archetype.h/.cpp, so the
// matching/template-assembly logic stays standalone-testable.

void Hs_LoadGroundedQuestionsFromDb();
void Hs_LoadGroundedTemplatesFromDb();

#endif // MOD_HS_GROUNDED_STORE_H
