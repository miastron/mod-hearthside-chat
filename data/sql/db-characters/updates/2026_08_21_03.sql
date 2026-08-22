-- Claude/PLAN-engagement.md (2026-08-21): the opener system's fifth trigger,
-- "prolonged proximity at a shared quest objective/flight master" (PLAN.md
-- §3), built alongside the engagement-follow-up feature since both share
-- hs_engagement.cpp's periodic scan WorldScript. New category plus a small
-- hand-authored seed, same shape and tone as the other four opener_*
-- categories in base/hside_corpus_category.sql / base/hside_corpus.sql.

INSERT IGNORE INTO `hside_corpus_category` (`name`, `tag_axis`, `card_gated`, `channel`, `is_opener`) VALUES
('opener_prolonged_proximity', 'none', 0, NULL, 1);

INSERT INTO `hside_corpus` (`name`, `text`) VALUES
('opener_prolonged_proximity', 'we keep ending up in the same spot, huh.'),
('opener_prolonged_proximity', 'small world, running into you out here again.'),
('opener_prolonged_proximity', 'you sticking around this area too?'),
('opener_prolonged_proximity', 'guess we had the same idea coming out here.');
