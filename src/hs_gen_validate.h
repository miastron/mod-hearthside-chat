#ifndef MOD_HS_GEN_VALIDATE_H
#define MOD_HS_GEN_VALIDATE_H

#include <string>
#include <vector>

// The generator's validation gate, shared by the idle-time bucket-filler
// (hs_generator.cpp) and `.hearthside capture` (hs_command.cpp): no
// automatic harvesting, both paths reuse this same gate. Pure string/logic
// checks only, no AzerothCore dependency or DB access, so this is
// standalone-testable, same pattern as hs_reflex.cpp/hs_grounded.cpp.
// Existing-row lookups and the actual INSERT live in hs_generator.cpp,
// which calls into this.

struct HsGenVerdict
{
    bool        accepted;
    std::string reason; // empty when accepted; a short machine-readable tag otherwise
};

// Quality gate (cheap regex checks): length, markdown/emoji/quote
// characters, modern slang, and wrong point of view (reads as a directed
// question rather than commentary; none of this module's seeded
// categories are openers). `allowQuestions` lets a caller opt out of just
// the question check: scripted bot-to-bot turns are natural
// back-and-forth dialogue and legitimately include questions, unlike every
// corpus category this gate was built for.
HsGenVerdict Hs_QualityGate(const std::string& candidate, bool allowQuestions = false);

// Placeholder discipline: if any of `existingRows` (the bucket's
// hand-authored exemplars) uses a placeholder, `candidate` must use one too,
// and any placeholder it does use must be recognized. Recognizes the
// universal placeholders always; `categoryCardGated` widens the recognized
// set to include the card-only ones (%main_focus etc), so a card-gated
// category won't need this file touched again later.
HsGenVerdict Hs_PlaceholderDiscipline(const std::string& candidate,
                                       const std::vector<std::string>& existingRows,
                                       bool categoryCardGated);

// Dedup: normalized-token Jaccard against existing rows in the same
// category, reject above ~0.6. Checked against every row in `existingRows`;
// the first near-duplicate found is the reason.
HsGenVerdict Hs_DedupCheck(const std::string& candidate, const std::vector<std::string>& existingRows);

// Scripted bot-to-bot dialogue's own placeholder discipline, a smaller,
// separate gate from Hs_PlaceholderDiscipline above, since scripts have no
// `existingRows` exemplar set to key off of and use their own token
// vocabulary (the %my_*/%other_* personal facts, resolved at delivery time
// by hs_corpus.h's Hs_ResolveScriptPlaceholders). Rejects a turn using any
// %-shaped token outside that vocabulary, since script turns have no
// fallback "drop the line" path once the model has committed to a whole
// exchange around it. A turn with no placeholder is always accepted.
HsGenVerdict Hs_ScriptPlaceholderDiscipline(const std::string& candidate);

// Runs all three gates in cost order (quality, placeholder, dedup) and stops
// at the first failure: what both callers actually call.
HsGenVerdict Hs_EvaluateCandidate(const std::string& candidate,
                                   const std::vector<std::string>& existingRows,
                                   bool categoryCardGated);

// True if `text` contains a `%word`-shaped placeholder token. Exposed
// individually (beyond Hs_PlaceholderDiscipline) because hs_generator.cpp's
// diversity-forcing prompt needs to tell the model "this bucket's lines use
// a placeholder" independent of evaluating any one candidate against it.
bool Hs_ContainsPlaceholder(const std::string& text);

// Pure normalized-token Jaccard similarity, [0.0, 1.0]. Exposed for the test
// harness; the ~0.6 threshold is applied inside Hs_DedupCheck.
double Hs_JaccardSimilarity(const std::string& a, const std::string& b);

#endif // MOD_HS_GEN_VALIDATE_H
