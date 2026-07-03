-- =============================================================================
-- reviewer_iters.sql — M-ITER (preregistration §2, RQ3/H3)
-- =============================================================================
-- Serves: RQ3 (query→RQ mapping: preregistration §7.1). Written 2026-07-03 as
-- part of the analysis-v2 corrective pass (the query was named in the frozen
-- protocol but never authored).
--
-- M-ITER definition (§2): reviewer request-changes cycles to reach the
-- objective gate.
--
-- MEASUREMENT CAVEAT (disclose wherever M-ITER is reported): the confirmatory
-- harness runs a SINGLE-VERDICT reviewer — the verdict is *measured* but no
-- rework loop executes (see the ITER_CAP note in scripts/bench-matrix.sh and
-- log/2026-07-03-instrument-review.md). M-ITER as recorded is therefore
-- "count of slices whose first review would have requested changes", a
-- *hypothetical* rework demand — not iterations actually performed or paid
-- for. 'unknown' verdicts (failed reviewer invocations) are reported
-- separately and excluded from the rate.
--
-- Usage:  sqlite3 <db> < metrics/reviewer_iters.sql
-- =============================================================================
select
  r.arm,
  sum(case when json_extract(e.payload, '$.verdict') = 'request-changes'
       then 1 else 0 end)                            as request_changes,
  sum(case when json_extract(e.payload, '$.verdict') = 'approve'
       then 1 else 0 end)                            as approves,
  sum(case when json_extract(e.payload, '$.verdict')
             not in ('approve', 'request-changes')
       then 1 else 0 end)                            as unknown_or_other,
  round(1.0 * sum(case when json_extract(e.payload, '$.verdict') = 'request-changes' then 1 else 0 end)
        / nullif(sum(case when json_extract(e.payload, '$.verdict') in ('approve','request-changes') then 1 else 0 end), 0), 3)
                                                     as request_changes_rate
from run_events e
join runs r on r.id = e.run_id
where e.kind = 'reviewer_decision'
  and r.status = 'completed'
group by r.arm
order by r.arm;
