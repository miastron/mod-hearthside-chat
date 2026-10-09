-- Category row for channel_general_band (level-banded General channel lines,
-- seeded in hside_corpus.sql). A new base/ file rather than an edit to
-- hside_corpus_category.sql for the same reason as
-- hside_corpus_category_ambient_group.sql: that file's hash is already
-- tracked on the deployed realm, and a new file applies exactly once.
-- INSERT IGNORE is safe here because hside_corpus_category is keyed on name.
-- Sorts after hside_corpus_category.sql ('.' < '_'), which creates the table.

INSERT IGNORE INTO `hside_corpus_category` (`name`, `tag_axis`, `card_gated`, `channel`, `is_opener`) VALUES
('channel_general_band', 'level_band', 0, 'general', 0);
