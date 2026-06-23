--[[ @meta
name: finalize-closeout
description: Deterministic closeout gate — evaluates plan readiness and closes without force.
phases: closeout
seam: planar run start/event/finish, planar plan closeout
--]]

-- finalize_closeout.lua — deterministic closeout-gate workflow (plan 638, M1).
--
-- Owns the highest-value, most-corrupting part of plan finalization: the DB
-- closeout gate. The gh/git ceremony (PR verify/merge, worktree/branch cleanup)
-- and `planar-agent reconcile` STAY in the janitor agent (the wrapper) — they
-- shell `gh`, `git`, and `planar-agent`, which are outside the `cli.planar`-only
-- surface available here.
--
-- ## Design: A1 — single closeout phase
--
-- One phase, one process. The caller invokes:
--
--   planar-execute run workflows/finalize_closeout.lua \
--       --phase closeout --args '{"plan_id":N}'
--
-- The phase:
--   1. Mints a run record so every closeout attempt is traceable.
--   2. Runs the gate in dry-run mode and emits a closeout-eval event.
--   3a. NOT READY → emits a blocked event, finishes the run aborted, and
--       returns flow.result({ready=false, blocked_by=...}). NEVER applies.
--   3b. READY → applies the closeout, emits a closed event, finishes the
--       run completed, and returns flow.result({ready=true, closed=true, ...}).
--
-- ## Rule 7 — STRUCTURAL never-force-close
--
-- The apply call (`plan closeout <plan_id>`) appears ONLY inside the
-- `gate.ready` branch below. It is unreachable from the not-ready path by
-- construction. The workflow cannot force-close a blocked plan because the
-- apply line of code is never reached on that branch.
--
-- ## Payload encoding
--
-- The sandbox nils `os`/`io`/`require`. There is no JSON encode helper in the
-- D7 host surface. Payloads for `run event --payload` are built as plain
-- strings using Lua string concatenation — a deliberate choice to stay spawn-
-- free and avoid depending on a library. The payload strings are valid JSON
-- for the values they carry (booleans are "true"/"false", strings are quoted).
--
-- ## Host surface used
--
--   cli.planar        — for `plan closeout` (apply; non-JSON, non-critical path)
--   cli.planar_json   — for `run start`, `run event`, `run finish` (JSON shapes)
--   flow.phase / flow.log / flow.fail / flow.result
--   ctx.args

-- ---------------------------------------------------------------------------
-- Helpers
-- ---------------------------------------------------------------------------

-- require_arg(name) reads ctx.args[name] or fails the phase loudly.
local function require_arg(name)
  local v = ctx.args[name]
  if v == nil then
    flow.fail("finalize_closeout: missing required --args field: " .. name)
  end
  return v
end

-- run_event(run_uid, kind, payload_str) appends a journal event. The
-- --payload value is a pre-built JSON string (or nil to omit the flag).
local function run_event(run_uid, kind, payload_str)
  if payload_str ~= nil then
    cli.planar({"run", "event", run_uid, "--kind", kind, "--payload", payload_str})
  else
    cli.planar({"run", "event", run_uid, "--kind", kind})
  end
end

-- ---------------------------------------------------------------------------
-- Phase: closeout
--
-- End-to-end closeout gate. Mints a run record, evaluates the hard gate,
-- and either closes the plan or surfaces the blockers — never both.
-- ---------------------------------------------------------------------------
function closeout()
  flow.phase("closeout")
  flow.log("finalize_closeout: closeout phase starting")

  local plan_id_num = require_arg("plan_id")
  local plan_id     = tostring(plan_id_num)

  -- 1. Open a run record so every gate evaluation is traceable.
  --    run start --json emits {"run_uid":"<hex>","plan_id":<n>,"arm":"<wf>"}.
  local run = cli.planar_json({
    "run", "start",
    "--plan",     plan_id,
    "--workflow", "finalize",
    "--json",
  })
  local run_uid = run.run_uid
  flow.log("finalize_closeout: run " .. run_uid .. " opened for plan " .. plan_id)

  -- 2. Emit an eval-start event so the run journal is self-describing.
  run_event(run_uid, "eval-start", nil)

  -- 3. Evaluate the gate in dry-run mode.
  --
  --    DESIGN: cli.planar_json on the dry-run exits 0 in BOTH ready and
  --    not-ready cases (--dry-run is a preview, not a gate). The structured
  --    {ready, blocked_by, ...} JSON is always available on stdout regardless
  --    of readiness. Read gate.ready to branch — no pcall needed.

  local gate = cli.planar_json({
    "plan", "closeout", plan_id,
    "--dry-run", "--json",
  })

  -- Emit the closeout-eval event with the structured verdict.
  if gate.ready then
    local payload = '{"ready":true,"blocked_by":[]}'
    run_event(run_uid, "closeout-eval", payload)
  else
    -- Build blocked_by array string from the structured gate response.
    -- Each element is a plain string; encode as JSON array of strings.
    local parts = {}
    if gate.blocked_by and #gate.blocked_by > 0 then
      for _, reason in ipairs(gate.blocked_by) do
        local r = tostring(reason or "unknown")
        r = r:gsub("\\", "\\\\"):gsub('"', '\\"'):gsub("\n", " "):gsub("\r", "")
        parts[#parts + 1] = '"' .. r .. '"'
      end
    end
    local blocked_arr = "[" .. table.concat(parts, ",") .. "]"
    local payload = '{"ready":false,"blocked_by":' .. blocked_arr .. '}'
    run_event(run_uid, "closeout-eval", payload)
  end

  -- -------------------------------------------------------------------------
  -- Rule 7 branch: NOT ready → aborted; NEVER call apply on this path.
  -- -------------------------------------------------------------------------
  if not gate.ready then
    -- Summarize the blockers for the event payload and the flow.result.
    local blockers = {}
    if gate.blocked_by then
      for _, reason in ipairs(gate.blocked_by) do
        local r = tostring(reason or "unknown")
        r = r:gsub("\\", "\\\\"):gsub('"', '\\"'):gsub("\n", " "):gsub("\r", "")
        blockers[#blockers + 1] = r
      end
    end
    local first_reason = blockers[1] or "plan not ready"

    -- Emit blocked event.
    local blocked_payload = '{"plan_id":' .. plan_id .. ',"reason":"' .. first_reason .. '"}'
    run_event(run_uid, "blocked", blocked_payload)

    -- Finish the run as aborted.
    cli.planar({"run", "finish", run_uid, "--status", "aborted"})

    flow.log("finalize_closeout: plan " .. plan_id .. " is NOT ready — run aborted, plan UNCHANGED")

    -- Return the not-ready result. The plan is UNCHANGED; closeout was NEVER
    -- applied. This is the structural Rule-7 guarantee.
    -- blocked_by now carries the real structured blocker list from the dry-run
    -- JSON (not the stderr fallback string).
    flow.result({
      ready       = false,
      blocked_by  = blockers,
      run_uid     = run_uid,
    })
    return
  end

  -- -------------------------------------------------------------------------
  -- Ready path: apply the closeout.
  -- The apply call is reachable ONLY from this branch (gate.ready == true).
  -- -------------------------------------------------------------------------
  flow.log("finalize_closeout: plan " .. plan_id .. " is READY — applying closeout")

  -- Apply (no --dry-run, no --json needed; non-zero would be unexpected here
  -- since we just passed the dry-run, but cli.planar will raise if it fails).
  cli.planar({"plan", "closeout", plan_id})

  -- Emit the closed event.
  local closed_payload = '{"plan_id":' .. plan_id .. ',"status":"done"}'
  run_event(run_uid, "closed", closed_payload)

  -- Finish the run as completed.
  cli.planar({"run", "finish", run_uid, "--status", "completed"})

  flow.log("finalize_closeout: plan " .. plan_id .. " closed; run " .. run_uid .. " completed")

  flow.result({
    ready   = true,
    closed  = true,
    run_uid = run_uid,
  })
end
