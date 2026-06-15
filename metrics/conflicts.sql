-- =============================================================================
-- conflicts.sql — M-CONF (preregistration §2, RQ3)
-- =============================================================================
-- Serves: RQ3 (query→RQ mapping: preregistration §7.1)
-- Tables: runs, run_events (kind='conflict')
-- Parameters: :run — the run_uid to measure
--
-- Usage:
--   sqlite3 <db> -cmd ".param set :run '<run_uid>'" < metrics/conflicts.sql
--
-- NOTE: conflict events are emitted by the harness at fan-in when a slice
-- produces a git merge conflict against the integration branch. This query
-- returns 0 against databases with no harness-emitted events — expected
-- during development. The SHAPE is correct and will produce results once the
-- harness emits conflict events.
--
-- Expected event payload (preregistration §2 M-CONF):
--   {"files": ["path/a.zig", "path/b.zig"]}  -- conflicting files in the slice
-- M-CONF counts the NUMBER OF SLICES that produced a conflict, not the
-- total number of conflicting files.
-- =============================================================================

-- ---------------------------------------------------------------------------
-- M-CONF: count of slices producing a git merge conflict at fan-in, per run.
-- (§2: "Count of slices that produce a git merge conflict against the
-- integration branch at fan-in, per run.")
-- ---------------------------------------------------------------------------
select
  r.run_uid,
  r.plan_id,
  r.arm,
  -- count of conflict events (one event per conflicting slice at fan-in)
  count(e.id)                                                       as conflicted_slice_count,
  -- total files across all conflicting slices (from the payload arrays)
  -- null when there are no conflict events
  sum(json_array_length(json_extract(e.payload, '$.files')))        as total_conflicted_files
from runs r
left join run_events e on e.run_id = r.id
  and e.kind = 'conflict'
where r.run_uid = :run
group by r.id, r.run_uid, r.plan_id, r.arm;
