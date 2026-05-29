-- handoffs.worktree_path / repo_root / branch: persist the source
-- session's worktree context on the handoff row so the resumer can
-- recover `cd <path>` even after the originating `agent_work_claims`
-- row has been released. Plan 297 M6 deferred this when the resumer
-- learned to read worktree_path from the live claim only; this
-- migration closes the cold-start recovery path. All three columns
-- are nullable — `planar handoff create` copies them from the active
-- claim when one exists, and leaves them NULL when no claim is held
-- (legacy / no-isolation flows). The columns mirror the names already
-- on `agent_work_claims` so the resumer's fallback projection lines
-- up field-for-field.
alter table handoffs add column worktree_path text;
alter table handoffs add column repo_root text;
alter table handoffs add column branch text;

insert into schema_migrations (version, description)
values (17, 'handoffs: add nullable worktree_path / repo_root / branch for cold-start resumer recovery');
