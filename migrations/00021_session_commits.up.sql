-- session_commits: commits attributed to a session, recorded at claim
-- terminal verbs, capture end, and by explicit operator attribution.
-- Metadata is denormalized so the row remains meaningful after worktrees
-- are deleted or history is rewritten.
create table session_commits (
  id           integer primary key autoincrement,
  session_id   integer not null references sessions(id) on delete cascade,
  claim_id     integer references agent_work_claims(id) on delete set null,
  sha          text not null,
  repo_root    text,
  branch       text,
  subject      text,
  author       text,
  committed_at text,
  recorded_at  text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  unique (session_id, sha)
);

create index ix_session_commits_session on session_commits(session_id);
create index ix_session_commits_sha on session_commits(sha);
create index ix_session_commits_claim on session_commits(claim_id) where claim_id is not null;

-- sessions.repo_root / head_sha_at_start: capture the repo and HEAD seen
-- when an operator session opens so `capture end` can later walk the
-- session-local commit window without recomputing the base state.
alter table sessions add column repo_root text;
alter table sessions add column head_sha_at_start text;

insert into schema_migrations (version, description)
values (21, 'session_commits table plus sessions repo_root/head_sha_at_start for session commit attribution');
