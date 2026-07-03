-- =============================================================================
-- coupling_density.sql — RQ4's x-axis (preregistration §7.1)
-- =============================================================================
-- Serves: RQ4 (query→RQ mapping: preregistration §7.1). Written 2026-07-03 as
-- part of the analysis-v2 corrective pass (log/2026-07-03-instrument-review.md:
-- RQ4 had been reported against qualitative low/coupled/mixed labels; the
-- frozen protocol defines a numeric density).
--
-- Definition (§7.1): coupling density = ratio of SHARED closure units to TOTAL
-- closure units for a plan — i.e. how much the plan's tasks' closures
-- (hyperedges) overlap.
--
-- Unit = one (path, symbol) pair in a task's EFFECTIVE derived closure
-- (roles 'modify' and 'reference'; 'transitive' rows are stored but excluded
-- from the effective closure by default — see docs/architecture.md, migration
-- 00026). A unit is SHARED when it appears in the closures of ≥ 2 of the
-- plan's tasks.
--
-- Reference values for the 2026-06 confirmatory corpus:
--   plan 659 → 0.051 (low)   plan 668 → 0.445 (high)   plan 678 → 0.323 (mixed)
--
-- Usage:  sqlite3 <db> < metrics/coupling_density.sql
-- =============================================================================
with units as (
  select t.plan_id,
         c.path || '::' || c.symbol as unit,
         count(distinct c.task_id)  as task_count
  from closures c
  join tasks t on t.id = c.task_id
  where c.role in ('modify', 'reference')
  group by t.plan_id, unit
)
select
  plan_id,
  count(*)                                          as total_units,
  sum(case when task_count >= 2 then 1 else 0 end)  as shared_units,
  round(1.0 * sum(case when task_count >= 2 then 1 else 0 end) / count(*), 3)
                                                    as coupling_density
from units
group by plan_id
order by plan_id;
