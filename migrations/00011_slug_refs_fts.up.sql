

-- ============================================================
-- Slug columns (nullable, per-kind unique indexes)
-- ============================================================

alter table artifacts      add column slug text;
alter table tasks          add column slug text;
alter table questions      add column slug text;
alter table test_scenarios add column slug text;
alter table decisions      add column slug text;

create unique index ux_artifacts_slug      on artifacts(slug)      where slug is not null;
create unique index ux_tasks_slug          on tasks(slug)          where slug is not null;
create unique index ux_questions_slug      on questions(slug)      where slug is not null;
create unique index ux_test_scenarios_slug on test_scenarios(slug) where slug is not null;
create unique index ux_decisions_slug      on decisions(slug)      where slug is not null;

-- ============================================================
-- FTS5 virtual tables (one per searchable entity kind)
-- ============================================================
--
-- Six FTS5 indexes — one per entity kind that has a title and body. Each
-- stores its own copy of (title, body) rather than pointing back at the
-- source table via external content. We tried content='<table>' first but
-- the plans table calls its body column 'summary', and FTS5 external
-- content matches FTS columns to base columns by name — there's no rename
-- knob, so a plans_fts with column 'body' tries to read plans.body and
-- fails. Self-contained content trades a small amount of storage for a
-- uniform shape across all six kinds (and decouples the FTS schema from
-- the source-of-truth column names should they evolve).
--
-- Tokenizer is unicode61 (Unicode case-fold + diacritic-stripping); this
-- is FTS5's most permissive built-in and fits our mixed natural-language
-- / identifier corpus.

create virtual table plans_fts using fts5(
  title, body,
  tokenize='unicode61'
);

create virtual table tasks_fts using fts5(
  title, body,
  tokenize='unicode61'
);

create virtual table questions_fts using fts5(
  title, body,
  tokenize='unicode61'
);

create virtual table test_scenarios_fts using fts5(
  title, body,
  tokenize='unicode61'
);

create virtual table decisions_fts using fts5(
  title, body,
  tokenize='unicode61'
);

create virtual table artifacts_fts using fts5(
  title, body,
  tokenize='unicode61'
);

-- ============================================================
-- Triggers — keep each FTS index in sync with its source table
-- ============================================================
--
-- For each base table: AFTER INSERT, AFTER UPDATE (delete-by-rowid + reinsert
-- to keep the inverted index correct), and AFTER DELETE. The DELETE form
-- (`delete from <fts> where rowid = ?`) is the standard way to evict from a
-- non-contentless FTS5 table; the special "INSERT(<fts>, 'delete', ...)"
-- form is reserved for external-content tables and would corrupt a
-- self-contained index. Plans use plans.summary as the body.

-- Each CREATE TRIGGER body contains multiple inner statements separated by
-- semicolons. Goose's default per-`;` statement splitter would chop those
-- bodies into invalid fragments, so each trigger is wrapped in explicit
-- statement-begin / statement-end markers per the goose SQL migration
-- format (annotation lines below).

-- plans (body = summary)
create trigger plans_fts_ai after insert on plans begin
  insert into plans_fts(rowid, title, body)
    values (new.id, new.title, coalesce(new.summary, ''));
end;
create trigger plans_fts_ad after delete on plans begin
  delete from plans_fts where rowid = old.id;
end;
create trigger plans_fts_au after update on plans begin
  delete from plans_fts where rowid = old.id;
  insert into plans_fts(rowid, title, body)
    values (new.id, new.title, coalesce(new.summary, ''));
end;

-- tasks
create trigger tasks_fts_ai after insert on tasks begin
  insert into tasks_fts(rowid, title, body)
    values (new.id, new.title, coalesce(new.body, ''));
end;
create trigger tasks_fts_ad after delete on tasks begin
  delete from tasks_fts where rowid = old.id;
end;
create trigger tasks_fts_au after update on tasks begin
  delete from tasks_fts where rowid = old.id;
  insert into tasks_fts(rowid, title, body)
    values (new.id, new.title, coalesce(new.body, ''));
end;

-- questions
create trigger questions_fts_ai after insert on questions begin
  insert into questions_fts(rowid, title, body)
    values (new.id, new.title, coalesce(new.body, ''));
end;
create trigger questions_fts_ad after delete on questions begin
  delete from questions_fts where rowid = old.id;
end;
create trigger questions_fts_au after update on questions begin
  delete from questions_fts where rowid = old.id;
  insert into questions_fts(rowid, title, body)
    values (new.id, new.title, coalesce(new.body, ''));
end;

-- test_scenarios
create trigger test_scenarios_fts_ai after insert on test_scenarios begin
  insert into test_scenarios_fts(rowid, title, body)
    values (new.id, new.title, coalesce(new.body, ''));
end;
create trigger test_scenarios_fts_ad after delete on test_scenarios begin
  delete from test_scenarios_fts where rowid = old.id;
end;
create trigger test_scenarios_fts_au after update on test_scenarios begin
  delete from test_scenarios_fts where rowid = old.id;
  insert into test_scenarios_fts(rowid, title, body)
    values (new.id, new.title, coalesce(new.body, ''));
end;

-- decisions
create trigger decisions_fts_ai after insert on decisions begin
  insert into decisions_fts(rowid, title, body)
    values (new.id, new.title, coalesce(new.body, ''));
end;
create trigger decisions_fts_ad after delete on decisions begin
  delete from decisions_fts where rowid = old.id;
end;
create trigger decisions_fts_au after update on decisions begin
  delete from decisions_fts where rowid = old.id;
  insert into decisions_fts(rowid, title, body)
    values (new.id, new.title, coalesce(new.body, ''));
end;

-- artifacts
create trigger artifacts_fts_ai after insert on artifacts begin
  insert into artifacts_fts(rowid, title, body)
    values (new.id, new.title, coalesce(new.body, ''));
end;
create trigger artifacts_fts_ad after delete on artifacts begin
  delete from artifacts_fts where rowid = old.id;
end;
create trigger artifacts_fts_au after update on artifacts begin
  delete from artifacts_fts where rowid = old.id;
  insert into artifacts_fts(rowid, title, body)
    values (new.id, new.title, coalesce(new.body, ''));
end;

-- ============================================================
-- Post-migration re-index: populate FTS tables from existing rows
-- ============================================================
--
-- The triggers above only fire on subsequent DML. Existing rows in each
-- source table need a one-shot backfill into their FTS index. Done inline
-- in the same migration so a freshly-applied 0011 leaves the DB in a fully
-- searchable state without a separate post-migration step.

insert into plans_fts(rowid, title, body)
  select id, title, coalesce(summary, '') from plans;

insert into tasks_fts(rowid, title, body)
  select id, title, coalesce(body, '') from tasks;

insert into questions_fts(rowid, title, body)
  select id, title, coalesce(body, '') from questions;

insert into test_scenarios_fts(rowid, title, body)
  select id, title, coalesce(body, '') from test_scenarios;

insert into decisions_fts(rowid, title, body)
  select id, title, coalesce(body, '') from decisions;

insert into artifacts_fts(rowid, title, body)
  select id, title, coalesce(body, '') from artifacts;

-- ============================================================
-- Record the migration
-- ============================================================

insert into schema_migrations (version, description)
values (11, 'slug refs + FTS5 search');
