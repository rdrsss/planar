--[[ @meta
name: handoff
description: Traced end-of-session handoff — snapshot, create, validate, and return handoff record.
phases: handoff
seam: planar run start/event/finish, planar capture snapshot, planar handoff create/validate, planar resume validate
--]]

-- handoff.lua — traced handoff creation workflow (plan 638, M5).
--
-- Implements the end-of-session capture ritual: snapshot the current state,
-- create a handoff record, validate it, and return the validated handoff
-- record. Mirrors the pl-handoff skill.
--
-- ## Invocation
--
--   planar-execute run workflows/handoff.lua \
--       --phase handoff \
--       --args '{"task_id":<n>[,"vendor":"<slug>","note":"<text>"]}'
--
-- Required --args fields:
--   task_id  (int)    — the task to hand off
--
-- Optional --args fields:
--   vendor   (string) — target vendor for the handoff (e.g. "claude")
--   note     (string) — handoff note / status message for the next session
--
-- ## What It Does
--
-- 1. Opens a trace run so the handoff attempt is durable-observable.
-- 2. Takes a context snapshot for the task via `capture snapshot`.
-- 3. Creates a handoff record from the snapshot via `handoff create`.
-- 4. Validates the handoff via `handoff validate` (pending → validated).
-- 5. Validates resumability via `resume validate`.
-- 6. Finishes the trace run.
-- 7. Returns the validated handoff record + resumability status.
--
-- ## Output (flow.result)
--
--   handoff_id      (int)     — the created handoff row id
--   snapshot_id     (int)     — the snapshot the handoff was built from
--   status          (string)  — handoff status after validation ("validated")
--   task_id         (int)     — the task the handoff is for
--   resumable       (bool)    — whether resume validate passed
--   run_uid         (string)  — the trace run uid
--
-- ## Tracing
--
-- A trace run is opened against the task's anchor plan. The run records:
--   "snapshot-created"  — after `capture snapshot` succeeds
--   "handoff-created"   — after `handoff create` succeeds
--   "handoff-validated" — after `handoff validate` succeeds
--   "resume-validated"  — after `resume validate` succeeds
--
-- The run is finished "completed" on success. On error in any step, the
-- workflow raises (cli.planar_json raises on non-zero exit) and the run
-- is left open — the operator can inspect it via `run show`.
--
-- ## Rule: single write path
--
-- The only state-mutation calls in this workflow are:
--   capture snapshot  (creates a context_snapshots row)
--   handoff create    (creates a handoffs row)
--   handoff validate  (flips handoffs.status pending → validated)
-- All other calls are read-only. This mirrors the pl-handoff skill contract.
--
-- ## Host surface used
--
--   cli.planar_json  — capture snapshot, handoff create/validate,
--                      task show, resume validate, run start/finish/event
--   cli.planar       — run finish, run event (non-json calls)
--   flow.phase / flow.log / flow.result / flow.fail
--   ctx.args

-- ---------------------------------------------------------------------------
-- Helpers
-- ---------------------------------------------------------------------------

local function require_arg(name)
  local v = ctx.args[name]
  if v == nil then
    flow.fail("handoff: missing required --args field: " .. name)
  end
  return v
end

local function escape_json_str(s)
  s = tostring(s or "")
  s = s:gsub("\\", "\\\\")
  s = s:gsub('"', '\\"')
  s = s:gsub("\n", " ")
  s = s:gsub("\r", "")
  return s
end

local function run_event(run_uid, kind, payload_str)
  if payload_str ~= nil then
    cli.planar({"run", "event", run_uid, "--kind", kind, "--payload", payload_str})
  else
    cli.planar({"run", "event", run_uid, "--kind", kind})
  end
end

-- ---------------------------------------------------------------------------
-- Phase: handoff
--
-- Full capture + create + validate + resume-validate ritual. Traced.
-- ---------------------------------------------------------------------------
function handoff()
  flow.phase("handoff")
  flow.log("handoff: starting capture ritual")

  local task_id_num = require_arg("task_id")
  local task_id     = tostring(task_id_num)
  local vendor      = ctx.args.vendor  -- may be nil
  local note        = ctx.args.note    -- may be nil

  -- 1. Look up the task's plan_id so we can open a trace run.
  --    `task show --json` returns {id, plan_id, title, status, ...}.
  local task_info = cli.planar_json({"task", "show", "--json", task_id})
  local plan_id   = tostring(task_info.plan_id)
  flow.log("handoff: task " .. task_id .. " belongs to plan " .. plan_id)

  -- 2. Open a trace run so the handoff attempt is durable-observable.
  local run = cli.planar_json({
    "run", "start",
    "--plan",     plan_id,
    "--workflow", "handoff",
    "--json",
  })
  local run_uid = run.run_uid
  flow.log("handoff: trace run " .. run_uid .. " opened")

  -- 3. Take a context snapshot for the task.
  --    `capture snapshot --task <id> --json` returns {id, task_id, ...}.
  local snap_argv = {"capture", "snapshot", "--task", task_id, "--json"}
  if note ~= nil then
    snap_argv[#snap_argv + 1] = "--note"
    snap_argv[#snap_argv + 1] = tostring(note)
  end
  local snap = cli.planar_json(snap_argv)
  local snapshot_id     = snap.id
  local snapshot_id_str = tostring(snapshot_id)
  flow.log("handoff: snapshot " .. snapshot_id_str .. " created for task " .. task_id)

  -- Emit snapshot-created trace event.
  local snap_payload = '{"task_id":' .. task_id ..
                       ',"snapshot_id":' .. snapshot_id_str .. '}'
  run_event(run_uid, "snapshot-created", snap_payload)

  -- 4. Create the handoff record from the snapshot.
  --    `handoff create <snapshot-id> --json` returns {id, status, ...}.
  local create_argv = {"handoff", "create", snapshot_id_str, "--json"}
  if note ~= nil then
    create_argv[#create_argv + 1] = "--note"
    create_argv[#create_argv + 1] = tostring(note)
  end
  if vendor ~= nil then
    create_argv[#create_argv + 1] = "--vendor"
    create_argv[#create_argv + 1] = tostring(vendor)
  end
  local created      = cli.planar_json(create_argv)
  local handoff_id   = created.id
  local handoff_id_str = tostring(handoff_id)
  flow.log("handoff: handoff " .. handoff_id_str .. " created (status=" .. tostring(created.status) .. ")")

  -- Emit handoff-created trace event.
  local create_payload = '{"handoff_id":' .. handoff_id_str ..
                         ',"snapshot_id":' .. snapshot_id_str ..
                         ',"status":"' .. escape_json_str(tostring(created.status)) .. '"}'
  run_event(run_uid, "handoff-created", create_payload)

  -- 5. Validate the handoff (pending → validated).
  --    `handoff validate <id> --json` returns {id, status, ...}.
  local validated       = cli.planar_json({"handoff", "validate", handoff_id_str, "--json"})
  local validated_status = tostring(validated.status)
  flow.log("handoff: handoff " .. handoff_id_str .. " validated (status=" .. validated_status .. ")")

  -- Emit handoff-validated trace event.
  local validate_payload = '{"handoff_id":' .. handoff_id_str ..
                           ',"status":"' .. escape_json_str(validated_status) .. '"}'
  run_event(run_uid, "handoff-validated", validate_payload)

  -- 6. Validate resumability via `resume validate <task-id> --json`.
  --    Returns {task_id, resumable, ...}. Non-zero exit means not resumable;
  --    wrap in pcall so we record the outcome rather than crashing.
  local rv_ok, rv_or_err = pcall(function()
    return cli.planar_json({"resume", "validate", task_id, "--json"})
  end)
  local resumable = rv_ok and (rv_or_err.resumable == true)
  flow.log("handoff: resume validate resumable=" .. tostring(resumable))

  -- Emit resume-validated trace event.
  local rv_payload = '{"task_id":' .. task_id ..
                     ',"resumable":' .. tostring(resumable) .. '}'
  run_event(run_uid, "resume-validated", rv_payload)

  -- 7. Finish the trace run as completed.
  cli.planar({"run", "finish", run_uid, "--status", "completed"})
  flow.log("handoff: trace run " .. run_uid .. " finished completed")

  -- 8. Return the result.
  flow.result({
    handoff_id  = handoff_id,
    snapshot_id = snapshot_id,
    status      = validated_status,
    task_id     = task_id_num,
    resumable   = resumable,
    run_uid     = run_uid,
  })
end
