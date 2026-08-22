#ifndef MOD_HS_GENERATOR_H
#define MOD_HS_GENERATOR_H

#include "hs_gen_validate.h"

#include <cstdint>
#include <string>
#include <vector>

// The idle-time generator. Runs on its own background thread, only ever
// firing a generation call when Hs_IsReactiveIdle() (hs_queue.h) says the
// reactive tier has nothing queued and isn't mid-request -- generation is
// always the lowest-priority work. Picks one under-quota (category,
// tag-bucket) at a time (quotas are per bucket, never global), generates one
// candidate line against it, and runs it through hs_gen_validate.h's gate
// before inserting.
//
// Scoped to the tag axes the seeded content actually uses (none, class,
// level_band) -- same none/faction/zone scoping as hs_corpus.h's selection
// path. Priority order across work types: cards first, then the script
// reserve, then corpus buckets.

void Hs_GeneratorStartup();
void Hs_GeneratorShutdown();

// Rows successfully inserted since this worldserver process started --
// read-only counter for `.hearthside status`, since the generator has no
// other visible signal when nothing is happening in-game.
uint32_t Hs_GeneratorRowsAddedThisSession();

// Unconsumed rows in hside_script right now. Backs `.hearthside status`
// (hs_command.cpp) so a dry reserve (bots go quiet to each other) is visible
// rather than discovered by noticing.
uint32_t Hs_ScriptReserveDepth();

// The tag_axis of a category ("none" | "class" | "level_band" | "faction" |
// "zone"), or empty if the category doesn't exist. Backs `.hearthside
// capture` (hs_command.cpp), which needs to know what tag value to derive
// from the captured bot's own state before calling Hs_TryInsertCorpusRow.
std::string Hs_LookupCategoryAxis(const std::string& category);

// Shared insert path, running the full validation gate. Both the generator
// loop and `.hearthside capture` call this; the only difference between them
// is who supplies candidateText, model, and promptVersion. tagColumn/
// tagValueSql are empty for a tag_axis "none" category; otherwise tagColumn
// is "class_tag"/"level_band_tag" and tagValueSql is a value already safe to
// drop into a WHERE clause (an integer string, or a single-quoted band
// label -- never raw player input).
HsGenVerdict Hs_TryInsertCorpusRow(const std::string& category, const std::string& tagColumn,
                                    const std::string& tagValueSql, const std::string& candidateText,
                                    const std::string& model, const std::string& promptVersion);

// Eviction is by exposure first, age second. Trims every (category, bucket)
// whose row count exceeds Generator.RowsPerBucket back down to quota,
// removing the most-exposed rows first (generated_at ASC as the tiebreaker
// for equal exposure, hand-authored/NULL rows protected last). Runs
// independently of the generator's enable flag (hs_main.cpp's
// HsCorpusLifecycleWorldScript) -- a bucket can go over quota via
// `.hearthside capture` or a lowered RowsPerBucket even while generation
// itself is off. Returns the number of rows evicted, for `.hearthside
// status`/debug logging.
//
// Deliberately mechanical and exposure-only: age-based eviction (a row gone
// unused for months) is a named gap, not built here, since there's no
// concrete threshold to build it against yet.
uint32_t Hs_RunEvictionSweep();

// Rows evicted since this worldserver process started -- `.hearthside
// status`, same shape as Hs_GeneratorRowsAddedThisSession.
uint32_t Hs_RowsEvictedThisSession();

// Bulk-evicts by generation run. A run is identified by its prompt_version
// tag, so this is a single DELETE keyed on that column -- no category/bucket
// scoping needed, since a bad run typically spans many buckets. Returns the
// number of rows deleted.
uint32_t Hs_EvictGenerationRun(const std::string& promptVersion);

// The most recent rows for a category (optionally narrowed to one
// prompt_version), for a GM/operator to eyeball a generation run's actual
// output before deciding whether to evict it -- the read counterpart to
// Hs_EvictGenerationRun. Capped at `limit` rows, most-recent (generated_at,
// falling back to id) first.
struct HsCorpusReviewRow
{
    std::string text;
    uint32_t    timesUsed;
    std::string model;         // "" for hand-authored
    std::string promptVersion; // "" for hand-authored
};
std::vector<HsCorpusReviewRow> Hs_ReviewCorpusRows(const std::string& category, const std::string& promptVersion, uint32_t limit);

#endif // MOD_HS_GENERATOR_H
