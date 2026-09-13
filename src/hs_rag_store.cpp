#include "hs_rag_store.h"
#include "hs_config.h"
#include "hs_log.h"
#include "hs_rag.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "QueryResult.h"

#include <cctype>
#include <string>
#include <vector>

namespace
{
    // hside_rag.keywords is a comma-separated list because the retriever
    // wants an ordered vector of authored handles and a side table would buy
    // nothing: nothing ever queries by a single keyword, the whole list is
    // read together on every load and never written by the server.
    //
    // Trims surrounding spaces and drops empties, so a hand-edited row with
    // "a, b,, c" loads as three handles rather than four with one blank --
    // a blank normalizes to no terms and would silently inflate the entry's
    // handleCount, weakening its own specificity bonus.
    std::vector<std::string> SplitKeywords(const std::string& csv)
    {
        std::vector<std::string> out;
        size_t start = 0;

        while (start <= csv.size())
        {
            size_t comma = csv.find(',', start);
            if (comma == std::string::npos)
                comma = csv.size();

            size_t b = start;
            size_t e = comma;
            while (b < e && std::isspace(static_cast<unsigned char>(csv[b]))) ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(csv[e - 1]))) --e;

            if (e > b)
                out.push_back(csv.substr(b, e - b));

            if (comma == csv.size())
                break;
            start = comma + 1;
        }

        return out;
    }
}

void Hs_LoadRagFromDb()
{
    QueryResult result = CharacterDatabase.Query("SELECT id, title, content, keywords FROM hside_rag");

    // `tags` is deliberately not selected: data/rag/README.md documents it as
    // carried for future filtering and parsed by nothing, and selecting a
    // column into a field HsRagEntry does not have would just invite someone
    // to assume it does something.

    if (!result)
    {
        // Not an error the module can recover from by guessing: an empty
        // table means every retrieval misses, which degrades to "no reference
        // block in the prompt" -- exactly the pre-RAG behaviour, not a crash.
        // Logged at ERROR anyway, because the silent failure mode here is a
        // bot that goes back to inventing answers, which reads as a model
        // problem rather than a missing seed.
        LOG_ERROR(kHsLog,
            "[HearthsideChat] hside_rag returned no rows -- world-knowledge retrieval is off and "
            "bots will answer game questions from the model's own priors. Check that "
            "base/hside_rag.sql installed (regenerate it with data/rag/generate_rag_sql.py).");
        Hs_SetRagTable({});
        return;
    }

    std::vector<HsRagEntry> rows;
    do
    {
        HsRagEntry entry;
        entry.id       = (*result)[0].Get<std::string>();
        entry.title    = (*result)[1].Get<std::string>();
        entry.content  = (*result)[2].Get<std::string>();
        entry.keywords = SplitKeywords((*result)[3].Get<std::string>());

        if (entry.id.empty() || entry.title.empty() || entry.content.empty())
        {
            LOG_ERROR(kHsLog,
                "[HearthsideChat] hside_rag row '{}' has an empty id, title or content -- skipped.", entry.id);
            continue;
        }

        rows.push_back(std::move(entry));
    } while (result->NextRow());

    Hs_SetRagTable(rows);

    if (g_HsDebugEnabled)
        LOG_INFO(kHsLog, "[HearthsideChat] Loaded {} world-knowledge entrie(s) from hside_rag.", rows.size());
}
