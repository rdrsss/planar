-- Restore the columns and their partial unique indexes exactly as 00011
-- (and the 00013 artifacts rebuild) declared them. The columns come back
-- empty: the up migration only ever ran against columns that were entirely
-- NULL, so nothing is lost on the roundtrip.
alter table artifacts add column slug text;
alter table questions add column slug text;
alter table test_scenarios add column slug text;
alter table decisions add column slug text;

create unique index ux_artifacts_slug on artifacts (slug)
where slug is not null;
create unique index ux_questions_slug on questions (slug)
where slug is not null;
create unique index ux_test_scenarios_slug on test_scenarios (slug)
where slug is not null;
create unique index ux_decisions_slug on decisions (slug)
where slug is not null;

delete from schema_migrations where version = 37;
