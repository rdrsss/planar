-- =============================================================================
-- rq1_touch_accuracy.sql — M-PREC / M-REC (preregistration §2, RQ1)
-- =============================================================================
-- Serves: RQ1 (query→RQ mapping: preregistration §7.1)
-- Tables: run_touches (self-join on kind='declared' vs. kind='actual')
-- Parameters: :run — the run_uid of the run to measure
--
-- Usage:
--   sqlite3 <db> -cmd ".param set :run '<run_uid>'" < metrics/rq1_touch_accuracy.sql
--
-- This file contains TWO queries:
--   1. Per-task precision/recall (the full distribution, per preregistration §2)
--   2. Aggregate macro-average + edge-case counts (per preregistration §2)
--
-- Both queries implement the frozen definitions verbatim.
-- =============================================================================

-- ---------------------------------------------------------------------------
-- QUERY 1: Per-task precision/recall distribution
-- ---------------------------------------------------------------------------
-- Preregistration §2 frozen rules implemented here:
--   * declared = run_touches rows where kind='declared', filtered to :run
--   * actual   = run_touches rows where kind='actual',  filtered to :run
--   * hit      = declared ∩ actual = rows matching on (task_id, path)
--   * precision = |declared ∩ actual| / |declared|
--     → NULL (excluded from precision aggregate) when |declared| = 0
--     (preregistration §2: "a task with |declared| = 0 has undefined
--     precision ... excluded from precision")
--   * recall    = |declared ∩ actual| / |actual|
--     → NULL (excluded from recall aggregate) when |actual| = 0
--     (preregistration §2: "a task with |actual| = 0 has undefined
--     recall; excluded")
--   * "the full per-task distribution is reported, not only the mean"
--     → this query is that full distribution; aggregate is QUERY 2.
-- ---------------------------------------------------------------------------
with d as (
  -- declared touch set for this run (§2: kind='declared' snapshot at start)
  select rt.task_id, rt.path
  from run_touches rt
  join runs r on r.id = rt.run_id
  where r.run_uid = :run
    and rt.kind = 'declared'
),
a as (
  -- actual touch set for this run (§2: kind='actual' from git diff at fan-in)
  select rt.task_id, rt.path
  from run_touches rt
  join runs r on r.id = rt.run_id
  where r.run_uid = :run
    and rt.kind = 'actual'
),
hit as (
  -- intersection: (task_id, path) present in both declared and actual sets
  -- (§2: |declared ∩ actual|)
  select d.task_id,
         count(*) as n
  from d
  join a using (task_id, path)
  group by d.task_id
),
dc as (
  -- per-task declared count (|declared|)
  select task_id, count(*) as n
  from d
  group by task_id
),
ac as (
  -- per-task actual count (|actual|)
  select task_id, count(*) as n
  from a
  group by task_id
),
all_tasks as (
  -- union of tasks appearing in either set (so edge-case tasks are included
  -- in the distribution even when absent from one side)
  select task_id from d
  union
  select task_id from a
)
select
  t.task_id,
  -- declared_count = 0 when the task has no declared touches (the *undeclared* case)
  coalesce(dc.n, 0)                           as declared_count,
  -- actual_count = 0 when the task touched nothing at fan-in
  coalesce(ac.n, 0)                           as actual_count,
  coalesce(hit.n, 0)                          as hit_count,
  -- §2: precision excluded (NULL) when |declared| = 0
  case when coalesce(dc.n, 0) = 0
       then null
       else coalesce(hit.n, 0) * 1.0 / dc.n
  end                                         as precision,
  -- §2: recall excluded (NULL) when |actual| = 0
  case when coalesce(ac.n, 0) = 0
       then null
       else coalesce(hit.n, 0) * 1.0 / ac.n
  end                                         as recall
from all_tasks t
left join dc using (task_id)
left join ac using (task_id)
left join hit using (task_id)
order by t.task_id;


-- ---------------------------------------------------------------------------
-- QUERY 2: Aggregate macro-average + edge-case counts
-- ---------------------------------------------------------------------------
-- Preregistration §2 frozen rules implemented here:
--   * "Aggregate = macro-average across included tasks"
--     → precision macro-avg = mean of per-task precision over tasks where
--       |declared| > 0 (included tasks for precision)
--     → recall macro-avg    = mean of per-task recall over tasks where
--       |actual| > 0 (included tasks for recall)
--   * undeclared_count = tasks with |declared| = 0 (excluded from precision,
--     reported separately; §2: "its count reported separately as undeclared")
--   * no_actual_count  = tasks with |actual| = 0 (excluded from recall,
--     reported separately; §2: "excluded, count reported")
-- ---------------------------------------------------------------------------
with d as (
  select rt.task_id, rt.path
  from run_touches rt
  join runs r on r.id = rt.run_id
  where r.run_uid = :run
    and rt.kind = 'declared'
),
a as (
  select rt.task_id, rt.path
  from run_touches rt
  join runs r on r.id = rt.run_id
  where r.run_uid = :run
    and rt.kind = 'actual'
),
hit as (
  select d.task_id, count(*) as n
  from d
  join a using (task_id, path)
  group by d.task_id
),
dc as (
  select task_id, count(*) as n from d group by task_id
),
ac as (
  select task_id, count(*) as n from a group by task_id
),
all_tasks as (
  select task_id from d
  union
  select task_id from a
),
per_task as (
  select
    t.task_id,
    coalesce(dc.n, 0)  as declared_count,
    coalesce(ac.n, 0)  as actual_count,
    coalesce(hit.n, 0) as hit_count,
    case when coalesce(dc.n, 0) = 0
         then null
         else coalesce(hit.n, 0) * 1.0 / dc.n
    end as precision,
    case when coalesce(ac.n, 0) = 0
         then null
         else coalesce(hit.n, 0) * 1.0 / ac.n
    end as recall
  from all_tasks t
  left join dc using (task_id)
  left join ac using (task_id)
  left join hit using (task_id)
)
select
  -- §2: macro-average precision over tasks where |declared| > 0
  avg(precision)                                      as macro_avg_precision,
  -- §2: macro-average recall over tasks where |actual| > 0
  avg(recall)                                         as macro_avg_recall,
  -- §2: "its count reported separately as undeclared"
  count(case when declared_count = 0 then 1 end)      as undeclared_count,
  -- §2: tasks with |actual| = 0, "excluded, count reported"
  count(case when actual_count = 0 then 1 end)        as no_actual_count,
  -- bookkeeping: tasks included in each average
  count(precision)                                    as precision_task_count,
  count(recall)                                       as recall_task_count,
  -- total task count (denominator for context)
  count(*)                                            as total_task_count
from per_task;
