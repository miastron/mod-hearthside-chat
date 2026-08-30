-- style_flags was reserved at table creation but never gained a consumer:
-- style is already fully deterministic from GUID+archetype (Hs_StyleCareForBot),
-- confirmed by an audit finding no INSERT/SELECT/UPDATE anywhere touches the
-- column. Dropping rather than leaving dead schema (Claude/archive/ISSUES.md).

ALTER TABLE `hside_identity` DROP COLUMN `style_flags`;
