--[[ @meta
name: health
description: Single-call health read — wraps planar health and returns db/schema/handoff status.
phases: health
seam: planar health --json
--]]

-- health.lua — single-call health read workflow (plan 638, M5).
--
-- Wraps `planar health --json` and returns the result via flow.result.
-- Mirrors the pl-health skill: reports db_ok, schema_current, integrity_ok,
-- inflight_tasks, resumable_tasks, not_resumable_tasks, pending_handoffs,
-- stale_handoffs, and overall verdict ("ok" / "degraded" / "critical").
--
-- ## Invocation
--
--   planar-execute run workflows/health.lua --phase health
--
-- No --args required or expected.
--
-- ## Output (flow.result)
--
-- The full planar health --json object is forwarded verbatim:
--
--   db_ok                  (bool)
--   schema_version         (int)
--   schema_target          (int)
--   schema_current         (bool)
--   migration_count        (int)
--   integrity_ok           (bool)
--   inflight_tasks         (int)
--   resumable_tasks        (int)
--   not_resumable_tasks    (int)
--   pending_handoffs       (int)
--   stale_handoffs         (int)
--   overall                (string) "ok" | "degraded" | "critical"
--
-- ## Tracing
--
-- No trace run is opened. Health is a pure single-call read with no
-- meaningful entity to associate a run record with. The call is
-- self-contained and the result is the entire output.
--
-- ## Host surface used
--
--   cli.planar_json  — health
--   flow.phase / flow.log / flow.result

-- ---------------------------------------------------------------------------
-- Phase: health
--
-- Shells `planar health --json`, logs the verdict, and returns the full
-- health object as flow.result.
-- ---------------------------------------------------------------------------
function health()
  flow.phase("health")
  flow.log("health: querying planar health")

  local h = cli.planar_json({"health", "--json"})

  flow.log("health: overall=" .. tostring(h.overall or "unknown") ..
           " db_ok=" .. tostring(h.db_ok) ..
           " schema_current=" .. tostring(h.schema_current))

  -- Forward the health object. All fields are scalars (bool/int/string);
  -- no empty-table hazard.
  flow.result({
    db_ok               = h.db_ok,
    schema_version      = h.schema_version,
    schema_target       = h.schema_target,
    schema_current      = h.schema_current,
    migration_count     = h.migration_count,
    integrity_ok        = h.integrity_ok,
    inflight_tasks      = h.inflight_tasks,
    resumable_tasks     = h.resumable_tasks,
    not_resumable_tasks = h.not_resumable_tasks,
    pending_handoffs    = h.pending_handoffs,
    stale_handoffs      = h.stale_handoffs,
    overall             = h.overall,
  })
end
