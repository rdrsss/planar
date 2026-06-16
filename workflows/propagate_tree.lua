-- propagate_tree.lua — resumable, traced propagate tree-walk workflow (plan 638, M3).
--
-- Walks a feature's entity tree and propagates each entity to an external
-- system one at a time, recording a trace event per entity. The walk
-- CONTINUES past single-entity failures (a failed POST does not abort the
-- tree). A re-run is idempotent: `ext propagate-one` skips entities that
-- already have an `external_links` row (op="skipped"), so only the missing
-- counterparts are created.
--
-- ## Invocation
--
--   planar-execute run workflows/propagate_tree.lua \
--       --phase propagate \
--       --args '{"plan_id":N,"system":"<slug>"}'
--
-- Required --args fields:
--   plan_id  (integer) — the anchor plan to walk
--   system   (string)  — the registered external system slug
--
-- Optional --args fields:
--   strategy (string)  — override the default strategy for the system
--
-- ## Run-finish status policy
--
-- The run is finished with status "completed" when every entity either
-- succeeded (op="created") or was already present (op="skipped"). If any
-- entity failed (pcall error from a non-zero `propagate-one` exit), the run
-- is finished with status "error". In both cases the walk completes — errors
-- are recorded per-entity and never abort the tree.
--
-- Rationale: "some failed" is not "aborted" — the workflow ran to completion
-- and produced a partial result. "error" is the correct terminal status for
-- a completed-but-imperfect run; "aborted" is reserved for runs that were
-- cut short before completing the walk.
--
-- ## Payload encoding
--
-- The sandbox nils `os`/`io`/`require`. There is no JSON encode helper in the
-- D7 host surface. Payloads for `run event --payload` are built as plain
-- strings using Lua string concatenation, following the same convention as
-- finalize_closeout.lua.
--
-- ## Host surface used
--
--   cli.planar_json  — descendants, propagate-one (with --json), run start/finish/event
--   cli.planar       — run finish, run event (non-json; simpler for status-only calls)
--   flow.phase / flow.log / flow.fail / flow.result
--   ctx.args

-- ---------------------------------------------------------------------------
-- Helpers
-- ---------------------------------------------------------------------------

-- require_arg(name) reads ctx.args[name] or fails the phase loudly.
local function require_arg(name)
  local v = ctx.args[name]
  if v == nil then
    flow.fail("propagate_tree: missing required --args field: " .. name)
  end
  return v
end

-- escape_json_str(s) makes a string safe for embedding inside a JSON string
-- literal: escapes backslashes, double-quotes, and strips newlines/CR.
local function escape_json_str(s)
  s = tostring(s or "")
  s = s:gsub("\\", "\\\\")
  s = s:gsub('"', '\\"')
  s = s:gsub("\n", " ")
  s = s:gsub("\r", "")
  return s
end

-- run_event(run_uid, kind, payload_str) appends a trace event. payload_str
-- is a pre-built JSON string or nil to omit the --payload flag.
local function run_event(run_uid, kind, payload_str)
  if payload_str ~= nil then
    cli.planar({"run", "event", run_uid, "--kind", kind, "--payload", payload_str})
  else
    cli.planar({"run", "event", run_uid, "--kind", kind})
  end
end

-- ---------------------------------------------------------------------------
-- Phase: propagate
--
-- Full tree-walk propagation. Mints a run record, calls plan descendants,
-- and for each entity calls ext propagate-one (wrapped in pcall so a single
-- entity failure never aborts the loop). Emits one trace event per entity.
-- ---------------------------------------------------------------------------
function propagate()
  flow.phase("propagate")
  flow.log("propagate_tree: propagate phase starting")

  local plan_id_num = require_arg("plan_id")
  local plan_id     = tostring(plan_id_num)
  local system      = require_arg("system")
  local strategy    = ctx.args.strategy  -- may be nil

  -- 1. Open a run record so every propagation attempt is traceable.
  local argv_start = {
    "run", "start",
    "--plan",     plan_id,
    "--workflow", "propagate",
    "--json",
  }
  local run = cli.planar_json(argv_start)
  local run_uid = run.run_uid
  flow.log("propagate_tree: run " .. run_uid .. " opened for plan " .. plan_id)

  -- 2. Emit a walk-start event so the run journal is self-describing.
  local start_payload = '{"plan_id":' .. plan_id .. ',"system":"' .. escape_json_str(system) .. '"}'
  run_event(run_uid, "walk-start", start_payload)

  -- 3. Fetch descendants in topological order.
  --    Returns [{kind, role, id, title}, ...] with the anchor plan first.
  local descendants = cli.planar_json({"plan", "descendants", plan_id, "--json"})
  flow.log("propagate_tree: got " .. tostring(#descendants) .. " descendant(s) for plan " .. plan_id)

  -- 4. Walk the tree: propagate each entity, recording one trace event per entity.
  --    pcall wraps each call so a non-zero exit (failed POST) is caught and
  --    recorded without aborting the loop.
  local created = 0
  local skipped = 0
  local errors  = {}

  for i = 1, #descendants do
    local entry     = descendants[i]
    local entity_ref = entry.kind .. ":" .. tostring(entry.id)

    flow.log("propagate_tree: propagating " .. entity_ref .. " (" .. entry.title .. ")")

    -- Build the argv for propagate-one. Strategy is optional.
    local argv = {
      "ext", "propagate-one", system,
      "--from", entity_ref,
      "--json",
    }
    if strategy ~= nil then
      argv[#argv + 1] = "--strategy"
      argv[#argv + 1] = strategy
    end

    local ok, result_or_err = pcall(function()
      return cli.planar_json(argv)
    end)

    if ok then
      -- result_or_err is the parsed PropagateOneResult table.
      local result = result_or_err
      local op = tostring(result.op or "unknown")

      if op == "created" then
        created = created + 1
      elseif op == "skipped" then
        skipped = skipped + 1
      end

      -- Emit a per-entity trace event.
      local ext_id = escape_json_str(tostring(result.external_id or ""))
      local payload = '{"entity":"' .. escape_json_str(entity_ref) .. '"' ..
                      ',"op":"' .. escape_json_str(op) .. '"' ..
                      ',"external_id":"' .. ext_id .. '"}'
      run_event(run_uid, "entity-propagated", payload)
      flow.log("propagate_tree: " .. entity_ref .. " -> " .. op .. " (" .. ext_id .. ")")
    else
      -- pcall caught a Lua error from the non-zero exit. Record and continue.
      local err_msg = escape_json_str(tostring(result_or_err or "propagate-one failed"))
      errors[#errors + 1] = entity_ref

      local payload = '{"entity":"' .. escape_json_str(entity_ref) .. '"' ..
                      ',"op":"error"' ..
                      ',"error":"' .. err_msg .. '"}'
      run_event(run_uid, "entity-propagated", payload)
      flow.log("propagate_tree: " .. entity_ref .. " FAILED: " .. err_msg)
    end
  end

  -- 5. Determine terminal run status.
  --    Policy: "error" when any entity failed; "completed" when all
  --    entities succeeded or were skipped. This is not "aborted" because the
  --    walk ran to completion — errors are partial-failure, not cut-short.
  local run_status = "completed"
  if #errors > 0 then
    run_status = "error"
  end

  -- 6. Emit a walk-done summary event.
  local error_list = ""
  for j = 1, #errors do
    if j > 1 then error_list = error_list .. "," end
    error_list = error_list .. '"' .. escape_json_str(errors[j]) .. '"'
  end
  local done_payload = '{"created":' .. tostring(created) ..
                       ',"skipped":' .. tostring(skipped) ..
                       ',"error_count":' .. tostring(#errors) ..
                       ',"errors":[' .. error_list .. ']}'
  run_event(run_uid, "walk-done", done_payload)

  -- 7. Finish the run.
  cli.planar({"run", "finish", run_uid, "--status", run_status})
  flow.log("propagate_tree: run " .. run_uid .. " finished with status " .. run_status)

  -- 8. Return the result payload.
  --
  -- Note: do not include `errors = {}` (empty table) in the result when there
  -- are no errors. The planar-execute JSON serializer's table-to-object path
  -- uses lua_next with a relative stack index that breaks after lua_pushnil is
  -- called, causing a null-pointer panic on empty tables. Omitting the field
  -- when nil lets the engine skip it; callers see `errors` absent (JSON) and
  -- use their default (empty slice). Non-empty error lists use the array path
  -- (rawlen > 0) and serialize safely.
  local result_table = {
    ok        = (#errors == 0),
    created   = created,
    skipped   = skipped,
    run_uid   = run_uid,
    run_status = run_status,
  }
  if #errors > 0 then
    result_table.errors = errors
  end
  flow.result(result_table)
end
