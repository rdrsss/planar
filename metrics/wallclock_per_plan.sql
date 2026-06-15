-- =============================================================================
-- wallclock_per_plan.sql — M-WALL (preregistration §2, RQ2)
-- =============================================================================
-- Serves: RQ2 (query→RQ mapping: preregistration §7.1)
-- Tables: runs, run_events
-- Parameters: :run — the run_uid to measure
--
-- Usage:
--   sqlite3 <db> -cmd ".param set :run '<run_uid>'" < metrics/wallclock_per_plan.sql
--
-- NOTE: dispatch/fan-in events are emitted by the harness. This query returns
-- null elapsed_seconds against databases with no harness-emitted events —
-- expected during development. The SHAPE is correct and will produce results
-- once the harness emits the expected event kinds.
--
-- Expected event kinds (preregistration §2 M-WALL):
--   kind='slice_dispatch' — emitted when the first slice is dispatched
--   kind='slice_fanin'    — emitted when a slice completes fan-in
-- M-WALL = seconds from min(slice_dispatch.created_at) to max(slice_fanin.created_at)
-- =============================================================================

-- ---------------------------------------------------------------------------
-- M-WALL: wall-clock seconds from first slice dispatch to last fan-in,
-- per run. (§2: "Seconds from first slice dispatch to last slice fan-in,
-- per run.")
-- ---------------------------------------------------------------------------
select
  r.run_uid,
  r.plan_id,
  r.arm,
  min(case when e.kind = 'slice_dispatch' then e.created_at end)   as first_dispatch_at,
  max(case when e.kind = 'slice_fanin'    then e.created_at end)   as last_fanin_at,
  -- elapsed seconds; null when dispatch or fanin events are absent
  round(
    (julianday(max(case when e.kind = 'slice_fanin'    then e.created_at end)) -
     julianday(min(case when e.kind = 'slice_dispatch' then e.created_at end)))
    * 86400.0,
    3
  )                                                                 as elapsed_seconds
from runs r
left join run_events e on e.run_id = r.id
  and e.kind in ('slice_dispatch', 'slice_fanin')
where r.run_uid = :run
group by r.id, r.run_uid, r.plan_id, r.arm;
