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
  --    IMPORTANT: `plan closeout --dry-run` exits NON-ZERO when the plan is
  --    NOT ready. The host's runAllowlisted raises a Lua error on non-zero
  --    exit. We wrap the call in pcall so we can handle the blocked case
  --    without crashing the phase. The JSON is always written to stdout before
  --    the non-zero exit, so cli.planar_json has already received it; but
  --    because the exit is non-zero runAllowlisted never returns — it raises.
  --
  --    Solution: call plan closeout --dry-run without --json first via pcall
  --    to detect readiness, then re-call with --json only on the ready path.
  --    Actually simpler: call with --json inside pcall; on error the plan is
  --    not ready (the error message carries the stderr). Then call show via
  --    a separate read for the blocked_by information.
  --
  --    Cleanest approach given the host surface: pcall the dry-run --json
  --    call; on success → gate table is our result; on error → plan is blocked
  --    and we read blocked_by by re-running with the same call (which will
  --    still emit JSON to stdout before exiting non-zero). We need the
  --    blocked_by array from the JSON, so we use a two-step:
  --      step A: pcall dry-run --json (success means ready=true)
  --      step B: if step A failed, re-run dry-run --json via pcall and parse
  --              the output — but runAllowlisted raises before returning stdout.
  --
  --    The only reliable way is: run dry-run WITHOUT --json (text mode) and
  --    then separately run plan show to read current status. But we lose the
  --    structured blocked_by array.
  --
  --    Best approach: use pcall around cli.planar_json for the dry-run. On
  --    the error path, run a separate `plan closeout --dry-run --json` via
  --    cli.planar (not _json) to get the raw stdout, then do a manual parse
  --    of the ready field and blocked_by. BUT cli.planar also raises on
  --    non-zero exit.
  --
  --    Root solution: since both cli.planar and cli.planar_json raise on
  --    non-zero exit, we must use pcall for the dry-run call and accept that
  --    on the error path we only have the Lua error string (which is the
  --    stderr from planar). The blocked_by list must come from a second
  --    dry-run call that we ALSO pcall. Both will raise, but we collect what
  --    we can from the error message.
  --
  --    Pragmatic design: pcall the dry-run _json call. On success → ready=true
  --    (gate table has blocked_by=[]). On failure → blocked; run a second
  --    pcall dry-run call to read the JSON gate table (both will raise, but
  --    runAllowlisted does write the error message with the stderr contents).
  --    The stderr on a blocked plan is the `plan <id> is not ready to close`
  --    message, not the JSON. The JSON went to stdout before the non-zero exit,
  --    but runAllowlisted only exposes the exit-error raiseError message which
  --    embeds stderr.
  --
  --    FINAL DESIGN: use a three-step approach:
  --    (a) pcall cli.planar_json on the dry-run -- if it succeeds, gate.ready
  --        must be true (ready plans exit 0). Proceed with apply.
  --    (b) If it fails (non-zero exit = not ready), surface blocked state.
  --        We know the plan is not ready. For the blocked_by list: the engine
  --        provides the stderr message in the Lua error string. We emit the
  --        raw error as the blocked_by payload rather than an empty array, so
  --        the caller has diagnostic information.

  local gate_ok, gate_or_err = pcall(function()
    return cli.planar_json({
      "plan", "closeout", plan_id,
      "--dry-run", "--json",
    })
  end)

  -- Emit the closeout-eval event with the verdict.
  if gate_ok then
    -- gate_or_err is the parsed gate table (ready=true, blocked_by=[]).
    local payload = '{"ready":true,"blocked_by":[]}'
    run_event(run_uid, "closeout-eval", payload)
  else
    -- gate_or_err is the Lua error string from the host (contains stderr).
    -- The plan is not ready.
    local err_msg = tostring(gate_or_err or "plan not ready")
    -- Sanitize the error message for use inside a JSON string: escape
    -- backslashes and double-quotes, strip newlines.
    err_msg = err_msg:gsub("\\", "\\\\"):gsub('"', '\\"'):gsub("\n", " "):gsub("\r", "")
    local payload = '{"ready":false,"blocked_by":["' .. err_msg .. '"]}'
    run_event(run_uid, "closeout-eval", payload)
  end

  -- -------------------------------------------------------------------------
  -- Rule 7 branch: NOT ready → aborted; NEVER call apply on this path.
  -- -------------------------------------------------------------------------
  if not gate_ok then
    local err_msg = tostring(gate_or_err or "plan not ready")
    err_msg = err_msg:gsub("\\", "\\\\"):gsub('"', '\\"'):gsub("\n", " "):gsub("\r", "")

    -- Emit blocked event.
    local blocked_payload = '{"plan_id":' .. plan_id .. ',"reason":"' .. err_msg .. '"}'
    run_event(run_uid, "blocked", blocked_payload)

    -- Finish the run as aborted.
    cli.planar({"run", "finish", run_uid, "--status", "aborted"})

    flow.log("finalize_closeout: plan " .. plan_id .. " is NOT ready — run aborted, plan UNCHANGED")

    -- Return the not-ready result. The plan is UNCHANGED; closeout was NEVER
    -- applied. This is the structural Rule-7 guarantee.
    flow.result({
      ready       = false,
      blocked_by  = {"gate blocked: see run " .. run_uid .. " events for details"},
      run_uid     = run_uid,
    })
    return
  end

  -- -------------------------------------------------------------------------
  -- Ready path: apply the closeout.
  -- The apply call is reachable ONLY from this branch (gate_ok == true).
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
