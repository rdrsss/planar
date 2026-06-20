--[[ @meta
name: status
description: Read-composition status — scope, plans, tasks, and questions composed into a scalar summary.
phases: status
seam: planar scope show, planar plan list, planar task list, planar question list, planar run start/event/finish
--]]

-- status.lua — read-composition status workflow (plan 638, M5).
--
-- Composes scope show + plan list (active, paused) + task list (todo, doing,
-- blocked) + question list (open) into a single structured summary. Mirrors
-- the pl-status skill output shape.
--
-- ## Invocation
--
--   planar-execute run workflows/status.lua \
--       --phase status \
--       [--args '{"scope":"<slug>","plan_id":<n>}']
--
-- Optional --args fields:
--   scope    (string) — passed as --scope to all list verbs (cwd-derived if absent)
--   plan_id  (int)    — when present, a trace run is opened against this plan
--                       so every status composition is durable-traceable
--
-- ## Output (flow.result)
--
-- Scalar fields only (engine limitation: nested object tables in flow.result
-- crash the writeLuaTableJson serializer). Arrays of objects are returned as
-- counts; the LLM/consumer composes the display from the raw planar CLI output.
--
--   scope_slug        (string)  — resolved scope slug from scope show
--   active_plan_count (int)     — number of active plans
--   paused_plan_count (int)     — number of paused plans
--   todo_task_count   (int)     — number of todo tasks
--   doing_task_count  (int)     — number of doing tasks
--   blocked_task_count(int)     — number of blocked tasks
--   open_question_count(int)    — number of open questions
--   summary           (string)  — human-readable one-liner
--   run_uid           (string)  — trace run uid (absent when plan_id not given)
--
-- ## Engine limitation note
--
-- The planar-execute writeLuaTableJson serializer panics on nested Lua tables
-- with string keys (objects) — lua_next with a relative stack index after
-- lua_pushnil becomes invalid. This affects any table containing sub-objects
-- (e.g. [{id=1, title="..."}]). To avoid this, this workflow returns only
-- scalar counts rather than the full entity lists.
--
-- ## Tracing
--
-- When ctx.args.plan_id is provided, the workflow mints a run record, emits
-- a "reads" event listing the verbs composed, and finishes the run. This
-- makes the composition durable and observable via `run show`. When plan_id
-- is absent, the workflow runs read-only with no trace.
--
-- ## Host surface used
--
--   cli.planar_json  — scope show, plan list, task list, question list,
--                      run start, run event, run finish
--   cli.planar       — run finish (non-json)
--   flow.phase / flow.log / flow.result
--   ctx.args

-- ---------------------------------------------------------------------------
-- Helpers
-- ---------------------------------------------------------------------------

-- escape_json_str makes a string safe for embedding inside a JSON string literal.
local function escape_json_str(s)
  s = tostring(s or "")
  s = s:gsub("\\", "\\\\")
  s = s:gsub('"', '\\"')
  s = s:gsub("\n", " ")
  s = s:gsub("\r", "")
  return s
end

-- run_event appends a trace event. payload_str is a pre-built JSON string or nil.
local function run_event(run_uid, kind, payload_str)
  if payload_str ~= nil then
    cli.planar({"run", "event", run_uid, "--kind", kind, "--payload", payload_str})
  else
    cli.planar({"run", "event", run_uid, "--kind", kind})
  end
end

-- ---------------------------------------------------------------------------
-- Phase: status
--
-- Composes scope + plans + tasks + questions into a result payload with
-- scalar counts. Returns counts rather than entity arrays to avoid the
-- nested-object-table serializer limitation.
-- ---------------------------------------------------------------------------
function status()
  flow.phase("status")
  flow.log("status: starting read-composition")

  local scope   = ctx.args.scope
  local plan_id = ctx.args.plan_id

  -- Build the base argv suffix for scoped list commands.
  local scope_suffix = {}
  if scope ~= nil then
    scope_suffix[#scope_suffix + 1] = "--scope"
    scope_suffix[#scope_suffix + 1] = tostring(scope)
  end

  -- 1. Resolve the current scope.
  local scope_argv = {"scope", "show", "--json"}
  for _, v in ipairs(scope_suffix) do scope_argv[#scope_argv + 1] = v end
  local scope_info = cli.planar_json(scope_argv)
  local scope_slug = ""
  if scope_info.resolved_scopes ~= nil and #scope_info.resolved_scopes > 0 then
    scope_slug = tostring(scope_info.resolved_scopes[1].slug or "")
  end
  flow.log("status: scope resolved to '" .. scope_slug .. "'")

  -- 2. Open a trace run if plan_id was provided.
  local run_uid = nil
  if plan_id ~= nil then
    local plan_id_str = tostring(plan_id)
    local run = cli.planar_json({
      "run", "start",
      "--plan",     plan_id_str,
      "--workflow", "status",
      "--json",
    })
    run_uid = run.run_uid
    flow.log("status: trace run " .. run_uid .. " opened for plan " .. plan_id_str)
  end

  -- 3. Compose the reads (collect counts only, not entity arrays).

  -- 3a. Plan list: active + paused.
  local active_argv = {"plan", "list", "--status", "active", "--json"}
  for _, v in ipairs(scope_suffix) do active_argv[#active_argv + 1] = v end
  local active_plans = cli.planar_json(active_argv)

  local paused_argv = {"plan", "list", "--status", "paused", "--json"}
  for _, v in ipairs(scope_suffix) do paused_argv[#paused_argv + 1] = v end
  local paused_plans = cli.planar_json(paused_argv)

  -- 3b. Task list: todo + doing + blocked.
  local todo_argv = {"task", "list", "--status", "todo", "--json"}
  for _, v in ipairs(scope_suffix) do todo_argv[#todo_argv + 1] = v end
  local todo_tasks = cli.planar_json(todo_argv)

  local doing_argv = {"task", "list", "--status", "doing", "--json"}
  for _, v in ipairs(scope_suffix) do doing_argv[#doing_argv + 1] = v end
  local doing_tasks = cli.planar_json(doing_argv)

  local blocked_argv = {"task", "list", "--status", "blocked", "--json"}
  for _, v in ipairs(scope_suffix) do blocked_argv[#blocked_argv + 1] = v end
  local blocked_tasks = cli.planar_json(blocked_argv)

  -- 3c. Question list: open.
  local qopen_argv = {"question", "list", "--status", "open", "--json"}
  for _, v in ipairs(scope_suffix) do qopen_argv[#qopen_argv + 1] = v end
  local open_questions = cli.planar_json(qopen_argv)

  -- Count results.
  local n_active     = #active_plans
  local n_paused     = #paused_plans
  local n_todo       = #todo_tasks
  local n_doing      = #doing_tasks
  local n_blocked    = #blocked_tasks
  local n_questions  = #open_questions
  local n_plans      = n_active + n_paused
  local n_tasks      = n_todo + n_doing + n_blocked

  flow.log("status: reads complete — plans(active=" .. tostring(n_active) ..
           " paused=" .. tostring(n_paused) ..
           ") tasks(todo=" .. tostring(n_todo) ..
           " doing=" .. tostring(n_doing) ..
           " blocked=" .. tostring(n_blocked) ..
           ") questions(open=" .. tostring(n_questions) .. ")")

  -- 4. Emit a trace event recording the composed reads.
  if run_uid ~= nil then
    local reads_payload = '{"verbs":["scope show","plan list --status active","plan list --status paused",' ..
                          '"task list --status todo","task list --status doing","task list --status blocked",' ..
                          '"question list --status open"],' ..
                          '"scope":"' .. escape_json_str(scope_slug) .. '"}'
    run_event(run_uid, "reads", reads_payload)
  end

  -- 5. Build a human-readable summary line.
  local summary = tostring(n_plans) .. " plan(s)"
  if n_tasks > 0 then
    summary = summary .. " · " .. tostring(n_tasks) .. " task(s)"
    if n_blocked > 0 then
      summary = summary .. " (" .. tostring(n_blocked) .. " blocked)"
    end
  end
  if n_questions > 0 then
    summary = summary .. " · " .. tostring(n_questions) .. " open question(s)"
  end

  -- 6. Finish the trace run (if open).
  if run_uid ~= nil then
    cli.planar({"run", "finish", run_uid, "--status", "completed"})
    flow.log("status: trace run " .. run_uid .. " finished")
  end

  -- 7. Return scalars only. No arrays/sub-tables — the engine's writeLuaTableJson
  --    panics on nested object tables (string-keyed sub-tables use lua_next with a
  --    relative idx after lua_pushnil, corrupting the stack).
  local result = {
    scope_slug          = scope_slug,
    active_plan_count   = n_active,
    paused_plan_count   = n_paused,
    todo_task_count     = n_todo,
    doing_task_count    = n_doing,
    blocked_task_count  = n_blocked,
    open_question_count = n_questions,
    summary             = summary,
  }
  if run_uid ~= nil then
    result.run_uid = run_uid
  end

  flow.log("status: done")
  flow.result(result)
end
