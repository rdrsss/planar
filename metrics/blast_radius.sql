-- =============================================================================
-- blast_radius.sql — M-BLAST (preregistration §2, RQ3/H3)
-- =============================================================================
-- Serves: RQ3 (query→RQ mapping: preregistration §7.1). Written 2026-07-03 as
-- part of the analysis-v2 corrective pass (the query was named in the frozen
-- protocol but never authored).
--
-- M-BLAST definition (§2): when a slice fails, the number of tasks surfaced
-- for rework (= slice size for the grouped arm; 1 for per-task arms).
--
-- MEASUREMENT CAVEAT (disclose wherever M-BLAST is reported): the confirmatory
-- harness has no rework loop, so no failure was ever actually *paid* — this
-- query reports the STRUCTURAL blast radius (the per-arm slice-size
-- distribution = the exposure a failure would have had), not realized rework.
-- Realized M-BLAST requires a harness with an enforcement loop (future work;
-- see log/2026-07-03-instrument-review.md, Finding on H3).
--
-- Usage:  sqlite3 <db> < metrics/blast_radius.sql
-- =============================================================================
select
  r.arm,
  count(*)                                                        as slices,
  round(avg(json_array_length(json_extract(e.payload, '$.tasks'))), 2)
                                                                  as avg_slice_size,
  max(json_array_length(json_extract(e.payload, '$.tasks')))      as max_slice_size,
  sum(case when json_array_length(json_extract(e.payload, '$.tasks')) > 1
       then 1 else 0 end)                                         as multi_task_slices
from run_events e
join runs r on r.id = e.run_id
where e.kind = 'slice_dispatch'
  and r.status = 'completed'
group by r.arm
order by r.arm;
