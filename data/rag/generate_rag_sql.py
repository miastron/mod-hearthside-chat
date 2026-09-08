#!/usr/bin/env python3
"""Regenerate data/sql/db-characters/base/hside_rag.sql from data/rag/*.json.

The JSON files are the authoring source; SQL is what ships (README.md's
"Where this data is going"). Run this after ANY edit under data/rag/:

    python data/rag/generate_rag_sql.py

Two things about the generated file that are deliberate and easy to get
wrong if you hand-edit it later:

1. It opens with DELETE FROM hside_rag, not a bare INSERT. Every base/*.sql
   file is SHA1-tracked, and AzerothCore re-applies one whose hash changed
   (repo CLAUDE.md, "How base/ vs updates/ actually behaves"). This data is
   *generated* and will change often, so a re-apply has to be a clean reload
   rather than a duplicate-key crash on the id PRIMARY KEY. That is the
   opposite of the other base/hside_*.sql files, which are hand-authored,
   frozen once shipped, and end in a plain INSERT.

2. Regenerating is therefore safe at any time, including on a deployed
   realm: the worst case is that the table is rewritten with the same rows.

Validates before writing and refuses to emit on a duplicate id, since
nothing catches that at load time and a duplicate silently double-weights a
topic in the retriever's IDF.
"""

import json
import pathlib
import sys

RAG = pathlib.Path(__file__).resolve().parent
OUT = RAG.parent / "sql" / "db-characters" / "base" / "hside_rag.sql"

HEADER = """-- hside_rag: static WoW world knowledge for the reactive tier and the
-- idle-time generator (src/hs_rag.h, src/hs_rag_store.cpp).
--
-- GENERATED FILE -- DO NOT HAND-EDIT.
-- Source of truth is data/rag/*.json; regenerate with:
--     python data/rag/generate_rag_sql.py
--
-- Unlike every other base/hside_*.sql in this module, this one is safe to
-- change and re-apply: it clears the table first, so AzerothCore's
-- re-apply-on-changed-hash behaviour reloads the corpus instead of failing
-- on a duplicate key. See that script's docstring for the full reasoning.

CREATE TABLE IF NOT EXISTS `hside_rag` (
  `id`       VARCHAR(64)  NOT NULL COMMENT 'Stable entry id, unique across every data/rag/*.json file. Also the retriever tie-break, so it must be stable across regenerations.',
  `title`    VARCHAR(128) NOT NULL COMMENT 'What the entry is about, as a player would say it. Weighted above keywords by the scorer, and the key Hs_RagContextForKeys addresses entries by.',
  `content`  TEXT         NOT NULL COMMENT 'The fact paragraph handed to the model, 2-4 plain sentences.',
  `keywords` TEXT         NOT NULL COMMENT 'Comma-separated retrieval handles. Keep tight: a long list dilutes the specificity bonus.',
  `tags`     VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'Carried for future filtering; read by nothing today.',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci
  COMMENT='Authored world-knowledge corpus retrieved into LLM prompts. Generated from data/rag/*.json.';

DELETE FROM `hside_rag`;

"""


def q(s):
    """MySQL string literal. Doubles the quote (ANSI) and escapes the
    backslash (needed while NO_BACKSLASH_ESCAPES is off, which is the
    default and the test realm's setting), so the result is correct under
    either sql_mode."""
    return "'" + s.replace("\\", "\\\\").replace("'", "''") + "'"


def main():
    entries = []
    seen = {}
    problems = []

    for path in sorted(RAG.glob("*.json")):
        try:
            rows = json.load(open(path, encoding="utf-8"))
        except json.JSONDecodeError as e:
            problems.append(f"{path.name}: invalid JSON -- {e}")
            continue

        for row in rows:
            for field in ("id", "title", "content", "keywords"):
                if field not in row:
                    problems.append(f"{path.name}: entry missing '{field}': {row.get('id', row)!r}")
            rid = row.get("id", "")
            if rid in seen:
                problems.append(f"duplicate id '{rid}' in {path.name} (first seen in {seen[rid]})")
            else:
                seen[rid] = path.name
            entries.append((path.name, row))

    if problems:
        print("Refusing to generate:", file=sys.stderr)
        for p in problems:
            print("  " + p, file=sys.stderr)
        return 1

    lines = [HEADER]
    current_file = None
    for filename, row in entries:
        if filename != current_file:
            current_file = filename
            lines.append(f"-- from {filename}\n")
        lines.append(
            "INSERT INTO `hside_rag` (`id`, `title`, `content`, `keywords`, `tags`) VALUES "
            f"({q(row['id'])}, {q(row['title'])}, {q(row['content'])}, "
            f"{q(','.join(row['keywords']))}, {q(','.join(row.get('tags', [])))});\n"
        )

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("".join(lines), encoding="utf-8")
    print(f"{len(entries)} entries from {len(set(f for f, _ in entries))} files -> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
