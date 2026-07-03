-- =============================================================================
-- parallelism_recovered.sql — M-PAR (preregistration §2, RQ2/H2)
-- =============================================================================
-- Serves: RQ2 (query→RQ mapping: preregistration §7.1). Written 2026-07-03 as
-- part of the analysis-v2 corrective pass (log/2026-07-03-instrument-review.md
-- Finding C: this query was named in the frozen protocol but never authored).
--
-- M-PAR definition (frozen, §2): count of tasks that the ELIGIBILITY arm
-- serializes (excluded from its parallel cohort) but the GROUPED arm places
-- into a co-located multi-task slice.
--
-- Evidence sources (per run):
--   * eligibility: slice_fanin events carry payload.cohort = true for cohort
--     members; serialized remainder tasks lack the flag.
--   * grouped: slice_dispatch payload.tasks arrays; a task is "co-located"
--     iff its slice has >1 task.
--
-- Slice shapes are structural (identical across reps of a plan), so M-PAR is
-- computed per plan over the union of runs. A plan where eligibility
-- serializes nothing (fully disjoint declared touches) has an undefined
-- recovery ratio — reported with serialized_count = 0 and NULL recovery.
--
-- Usage:  sqlite3 <db> < metrics/parallelism_recovered.sql
-- =============================================================================
with serialized as (
  -- tasks eligibility ran OUTSIDE the concurrent cohort, per plan
  select distinct r.plan_id, je.value as task_id
  from run_events e
  join runs r on r.id = e.run_id
  join json_each(json_extract(e.payload, '$.tasks')) je
  where e.kind = 'slice_fanin'
    and r.arm = 'eligibility'
    and coalesce(json_extract(e.payload, '$.cohort'), 0) != 1
),
colocated as (
  -- tasks grouped placed into a multi-task (co-located) slice, per plan
  select distinct r.plan_id, je.value as task_id
  from run_events e
  join runs r on r.id = e.run_id
  join json_each(json_extract(e.payload, '$.tasks')) je
  where e.kind = 'slice_dispatch'
    and r.arm = 'grouped'
    and json_array_length(json_extract(e.payload, '$.tasks')) > 1
)
select
  s.plan_id,
  count(s.task_id)                                   as serialized_count,
  count(c.task_id)                                   as recovered_count,
  case when count(s.task_id) > 0
       then round(1.0 * count(c.task_id) / count(s.task_id), 3)
       end                                           as m_par_recovery
from serialized s
left join colocated c
  on c.plan_id = s.plan_id and c.task_id = s.task_id
group by s.plan_id

union all

-- plans where eligibility serialized nothing (recovery undefined, reported)
select r.plan_id, 0, 0, null
from runs r
where r.arm = 'eligibility'
  and r.plan_id not in (
    select distinct r2.plan_id
    from run_events e2 join runs r2 on r2.id = e2.run_id
    where e2.kind = 'slice_fanin' and r2.arm = 'eligibility'
      and coalesce(json_extract(e2.payload, '$.cohort'), 0) != 1)
group by r.plan_id;
