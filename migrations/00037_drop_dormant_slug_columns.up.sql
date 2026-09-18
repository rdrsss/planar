-- Migration 00011 added a nullable `slug` column to five tables at once.
-- Only `tasks.slug` was ever read or written (roadmap `[slug: ...]`
-- annotations, `verifies: task:<slug>` citations, global collision checks).
-- The other four never held a value in a year of use and no statement in
-- the tree selects or binds them (task 6808, measured 2026-09-17). Drop
-- them so nobody rediscovers the column, assumes it is load-bearing, and
-- wires a setter for a value nothing can read back.
--
-- `plans.slug` (00001) and `annotations.slug` (00012) are live and untouched.
--
-- The partial unique indexes must go before the columns they cover:
-- `alter table ... drop column` refuses a column an index references.
drop index if exists ux_artifacts_slug;
drop index if exists ux_questions_slug;
drop index if exists ux_test_scenarios_slug;
drop index if exists ux_decisions_slug;

alter table artifacts drop column slug;
alter table questions drop column slug;
alter table test_scenarios drop column slug;
alter table decisions drop column slug;

insert into schema_migrations (version, description)
values (37, 'drop the four dormant slug columns added by 00011');
