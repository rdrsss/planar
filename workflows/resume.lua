--[[ @meta
name: resume
description: Task resume packet read — wraps planar resume and returns the full structured packet for agent handoff.
phases: resume
seam: planar resume <task_id> --json
--]]

-- resume.lua — task resume packet read workflow (plan 638, M5).
--
-- Wraps `planar resume <task_id> --json` and returns the full 8-section
-- resume packet via flow.result. The writeLuaTableJson nested-object panic
-- is fixed in plan 638 (lua_absindex captures the table slot before any
-- lua_push* call), so arrays of objects and nested tables now serialize
-- correctly.
--
-- ## Invocation
--
--   planar-execute run workflows/resume.lua \
--       --phase resume --args '{"task_id":<n>}'
--
-- Required --args fields:
--   task_id  (int) — the task to produce the resume packet for
--
-- ## Output (flow.result)
--
-- The full 8-section resume packet forwarded verbatim from planar resume --json:
--
--   identity         — {task_id, plan_id, title, status, scope_kind, scope_id}
--   state            — {status, next_action, last_action_at, last_action_body}
--   plan             — {plan_id, plan_title, completed[], current[], remaining[]}
--   operational_plane— {links[], refresh_note}
--   recent_activity  — [{session_id, prefix, body, created_at}, ...]
--   decisions        — [{id, title, status}, ...]
--   questions        — [{id, title, status, answer_body}, ...]
--   artifacts        — [{artifact_id, title, kind, relationship}, ...]
--   audit            — {session_id, vendor, started_at} or absent
--   active_claim     — {claim_id, claim_token, vendor, worktree_path, ...} or absent
--   from_handoff     — {handoff_id, worktree_path, ...} or absent
--
-- ## Tracing
--
-- No trace run is opened. Resume is a pure read — it does not write state.
-- The workflow is deterministic and idempotent; tracing adds no value here.
--
-- ## Error handling
--
-- When the task does not exist, `planar resume --json <id>` exits non-zero.
-- cli.planar_json raises a Lua error in that case; the engine exits non-zero
-- with the diagnostic in stderr. This is the correct behavior.
--
-- ## Host surface used
--
--   cli.planar_json  — resume
--   flow.phase / flow.log / flow.result / flow.fail
--   ctx.args

-- ---------------------------------------------------------------------------
-- Helpers
-- ---------------------------------------------------------------------------

local function require_arg(name)
  local v = ctx.args[name]
  if v == nil then
    flow.fail("resume: missing required --args field: " .. name)
  end
  return v
end

-- ---------------------------------------------------------------------------
-- Phase: resume
--
-- Shells `planar resume <task_id> --json` and forwards the full 8-section
-- packet as flow.result. The writeLuaTableJson nested-object panic is fixed
-- (lua_absindex), so nested arrays of objects now serialize correctly.
-- ---------------------------------------------------------------------------
function resume()
  flow.phase("resume")
  flow.log("resume: starting")

  local task_id_num = require_arg("task_id")
  local task_id     = tostring(task_id_num)
  flow.log("resume: fetching resume packet for task " .. task_id)

  -- planar resume <id> --json produces the full 8-section packet.
  -- cli.planar_json raises on non-zero exit (task not found, etc.).
  local packet = cli.planar_json({"resume", "--json", task_id})

  flow.log("resume: packet received for task " .. task_id)

  -- Log key identity+state fields for observability.
  local identity = packet.identity or {}
  local state    = packet.state    or {}
  local status_v      = tostring(state.status or "")
  local next_action_v = tostring(state.next_action or "")
  flow.log("resume: task=" .. task_id ..
           " status=" .. status_v ..
           " next_action='" .. next_action_v .. "'")

  -- Forward the packet verbatim. The engine's writeLuaTableJson now handles
  -- nested object tables and arrays of objects (lua_absindex fix, plan 638).
  flow.log("resume: done")
  flow.result(packet)
end
