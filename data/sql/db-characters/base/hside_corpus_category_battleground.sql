-- Battleground callouts (hs_event.cpp, "Battleground progress"): one
-- teammate's line in the battleground's raid chat. Canned rather than the
-- model: the tuned model has no rows for mid-match play and answered every
-- probe with "gg" (2026-10-14).
--
-- is_opener = 1 keeps these out of every random pool; only
-- Hs_SelectOpenerLine, called by name, reaches them. tag_axis none: the
-- corpus has no archetype axis, so the lines are written to fit any voice.
--
-- flag_* lines follow a flag capture or return in Warsong Gulch and Eye of the
-- Storm (bg_flag_capped_theirs_ey for Eye of the Storm, whose flag is fought
-- over at the center rather than defended at home). node_* lines are said by
-- a bot standing at or near a node, before the outcome, the way players call
-- it: inc as enemies close on a node its
-- side holds, help once they outnumber the defenders, safe when they are gone,
-- taking when its side outnumbers the other at a node it does not hold.
-- %node is the node's short name (lm, gm, fr, bet, sh gy, ...), %n how many
-- players: enemies for inc and help, allies for taking.
--
-- Safe to edit and re-apply, like hside_corpus.sql: the DELETE removes this
-- file's own hand-written rows before they are inserted again. A new
-- category added below must be added to that list too. The bg_base_*
-- categories were replaced by the node_* ones on 2026-10-14.

DELETE FROM `hside_corpus_category` WHERE `name` IN ('bg_base_taken_ours', 'bg_base_taken_theirs', 'bg_base_defended_ours');

INSERT IGNORE INTO `hside_corpus_category` (`name`, `tag_axis`, `card_gated`, `channel`, `is_opener`) VALUES
('bg_flag_capped_ours',   'none', 0, 'raid', 1),
('bg_flag_capped_theirs', 'none', 0, 'raid', 1),
('bg_flag_capped_theirs_ey', 'none', 0, 'raid', 1),
('bg_flag_returned_ours', 'none', 0, 'raid', 1),
('bg_node_inc',           'none', 0, 'raid', 1),
('bg_node_help',          'none', 0, 'raid', 1),
('bg_node_safe',          'none', 0, 'raid', 1),
('bg_node_taking',        'none', 0, 'raid', 1);

DELETE FROM `hside_corpus` WHERE `generated_at` IS NULL AND `name` IN (
  'bg_flag_capped_ours', 'bg_flag_capped_theirs', 'bg_flag_capped_theirs_ey', 'bg_flag_returned_ours',
  'bg_node_inc', 'bg_node_help', 'bg_node_safe', 'bg_node_taking',
  'bg_base_taken_ours', 'bg_base_taken_theirs', 'bg_base_defended_ours'
);

INSERT INTO `hside_corpus` (`name`, `text`) VALUES
('bg_flag_capped_ours', 'nice cap'),
('bg_flag_capped_ours', 'gj fc'),
('bg_flag_capped_ours', 'ez'),
('bg_flag_capped_ours', 'there we go'),
('bg_flag_capped_ours', 'gg fc'),
('bg_flag_capped_ours', 'nice'),
('bg_flag_capped_ours', 'one more'),
('bg_flag_capped_ours', 'yes'),
('bg_flag_capped_ours', 'lets go'),
('bg_flag_capped_ours', 'clutch'),

('bg_flag_capped_theirs', 'wheres d'),
('bg_flag_capped_theirs', 'wtf d'),
('bg_flag_capped_theirs', 'need d'),
('bg_flag_capped_theirs', 'ugh'),
('bg_flag_capped_theirs', 'come on'),
('bg_flag_capped_theirs', 'gg'),
('bg_flag_capped_theirs', 'nobody home?'),
('bg_flag_capped_theirs', 'kill their fc'),
('bg_flag_capped_theirs', 'd pls'),
('bg_flag_capped_theirs', 'ffs'),

('bg_flag_capped_theirs_ey', 'come on'),
('bg_flag_capped_theirs_ey', 'ugh'),
('bg_flag_capped_theirs_ey', 'ffs'),
('bg_flag_capped_theirs_ey', 'gg'),
('bg_flag_capped_theirs_ey', 'need more mid'),
('bg_flag_capped_theirs_ey', 'next one is ours'),
('bg_flag_capped_theirs_ey', 'win mid'),
('bg_flag_capped_theirs_ey', 'get the next one'),

('bg_flag_returned_ours', 'nice'),
('bg_flag_returned_ours', 'good save'),
('bg_flag_returned_ours', 'good'),
('bg_flag_returned_ours', 'nice return'),
('bg_flag_returned_ours', 'good job'),
('bg_flag_returned_ours', 'close one'),
('bg_flag_returned_ours', 'good d'),
('bg_flag_returned_ours', 'gj all'),

('bg_node_inc', 'inc %node'),
('bg_node_inc', 'inc %node %n'),
('bg_node_inc', '%node inc'),
('bg_node_inc', '%node inc %n'),
('bg_node_inc', '%n inc %node'),
('bg_node_inc', 'inc %node!!'),
('bg_node_inc', 'inc %node %n+'),

('bg_node_help', '%node needs help'),
('bg_node_help', 'help %node'),
('bg_node_help', '%node is going'),
('bg_node_help', 'losing %node'),
('bg_node_help', 'need help %node'),
('bg_node_help', '%node help!!'),
('bg_node_help', '%n at %node help'),

('bg_node_safe', '%node safe'),
('bg_node_safe', '%node clear'),
('bg_node_safe', '%node ok'),
('bg_node_safe', '%node safe now'),
('bg_node_safe', '%node good'),

('bg_node_taking', 'taking %node'),
('bg_node_taking', 'got %node'),
('bg_node_taking', '%node is ours'),
('bg_node_taking', 'capping %node'),
('bg_node_taking', '%node ours');
