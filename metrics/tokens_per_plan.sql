-- =============================================================================
-- tokens_per_plan.sql — M-TOK (preregistration §2, RQ2)
-- =============================================================================
-- Serves: RQ2 (query→RQ mapping: preregistration §7.1)
-- Tables: runs, run_events (kind='token_sample')
-- Parameters: :run — the run_uid to aggregate (omit WHERE clause to see all)
--
-- Usage:
--   sqlite3 <db> -cmd ".param set :run '<run_uid>'" < metrics/tokens_per_plan.sql
--
-- NOTE: token_sample events are emitted by the harness during agent dispatch.
-- This query returns 0/empty against databases with no harness-emitted events —
-- that is expected during development. The SHAPE is correct and will produce
-- results once the harness emits token_sample events.
--
-- Payload schema expected (preregistration §2 M-TOK):
--   {"in": <integer>, "out": <integer>}          -- raw billed tokens (primary)
--   {"in": <integer>, "out": <integer>,
--    "cache_in": <integer>, "cache_out": <integer>}  -- cache-adjusted (secondary)
-- =============================================================================

-- ---------------------------------------------------------------------------
-- M-TOK: sum of input + output tokens across ALL agents including reviewers,
-- per run. (§2: "Sum of input + output tokens across ALL agents in the run,
-- including reviewer agents. Primary figure is raw billed tokens;
-- cache-adjusted tokens are a secondary figure, reported alongside,
-- never substituted.")
-- ---------------------------------------------------------------------------
select
  r.run_uid,
  r.plan_id,
  r.arm,
  -- primary: raw billed tokens (input + output, per §2)
  coalesce(sum(
    json_extract(e.payload, '$.in') +
    json_extract(e.payload, '$.out')
  ), 0)                                                             as total_tokens_raw,
  -- secondary: cache-adjusted input + output where payload carries it
  -- (null when the payload does not include cache fields; §2: "secondary
  -- figure, reported alongside, never substituted")
  sum(
    coalesce(json_extract(e.payload, '$.cache_in'),
             json_extract(e.payload, '$.in')) +
    coalesce(json_extract(e.payload, '$.cache_out'),
             json_extract(e.payload, '$.out'))
  )                                                                 as total_tokens_cache_adjusted,
  count(e.id)                                                       as event_count
from runs r
left join run_events e on e.run_id = r.id
  and e.kind = 'token_sample'
where r.run_uid = :run
group by r.id, r.run_uid, r.plan_id, r.arm;
