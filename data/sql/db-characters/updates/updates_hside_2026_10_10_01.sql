-- Expert answers (2026-10-10): which world-knowledge domain an archetype is
-- expert in. hs_rag.h's HsRagEntry::expertContent is swapped in for an entry
-- of this domain when the bot is talking to a player who has earned it
-- (grouped, same guild, friended, or a shared memory beyond first meeting:
-- hs_queue.cpp's WorkerLoop). Empty = no expertise, the normal paragraph.
ALTER TABLE `hside_archetype`
  ADD COLUMN `expertise` VARCHAR(16) NOT NULL DEFAULT ''
  COMMENT 'gold|pve|pvp|general, or empty: matches hside_rag.expert_domain';

UPDATE `hside_archetype` SET `expertise` = 'gold'    WHERE `enum_name` = 'TRADER';
UPDATE `hside_archetype` SET `expertise` = 'pve'     WHERE `enum_name` = 'RAIDER_SERIOUS';
UPDATE `hside_archetype` SET `expertise` = 'pvp'     WHERE `enum_name` = 'PVP_SERIOUS';
UPDATE `hside_archetype` SET `expertise` = 'general' WHERE `enum_name` IN ('MENTOR', 'GRUMPY_VETERAN');
