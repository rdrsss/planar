--[[ @meta
name: resume
description: Task resume packet read — wraps planar resume and returns key fields for agent handoff.
phases: resume
seam: planar resume <task_id> --json
--]]

-- resume.lua — task resume packet read workflow (plan 638, M5).
--
-- Wraps `planar resume <task_id> --json` and returns the key fields from the
-- structured resume packet via flow.result. Mirrors the pl-resume skill: the
-- packet contains identity, state, plan position, operational plane, recent
-- activity, decisions, artifacts, and audit footer.
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
-- Key fields from the resume packet (scalars + non-empty arrays only):
--
--   task_id       (int)    — task id
--   plan_id       (int)    — anchor plan id (absent when task has no plan)
--   title         (string) — task title
--   status        (string) — task status
--   next_action   (string) — next action text
--   scope_kind    (string) — scope kind (e.g. "global", "project")
--   worktree_path (string) — active claim worktree path (absent when empty)
--
-- ## Why fields are extracted rather than forwarded verbatim
--
-- The planar-execute writeLuaTableJson engine panics when it encounters an
-- empty Lua table (which represents both empty arrays [] and empty objects {}
-- from the JSON deserializer). The resume packet contains many empty arrays
-- (completed, current, remaining, recent_activity, decisions, questions,
-- artifacts, links) and null values that become empty sub-tables. Forwarding
-- the packet verbatim via flow.result(packet) triggers the panic.
--
-- Solution: extract the scalar fields we need and omit empty array sections.
-- The pl-resume skill reads the full packet from `planar resume --json`
-- directly; this workflow surfaces the same key identity+state fields for
-- machine consumers that need the resume context programmatically.
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
-- Shells `planar resume <task_id> --json` and returns the key identity+state
-- fields from the packet as flow.result, omitting empty sub-tables that
-- would cause the engine to panic.
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

  -- Extract scalar fields from the packet. We read each sub-section
  -- individually to pull out the scalar leaves, avoiding empty tables.
  --
  -- Section 1: identity
  local identity   = packet.identity or {}
  local task_id_v  = identity.task_id or task_id_num
  local plan_id_v  = identity.plan_id   -- may be nil
  local title_v    = tostring(identity.title or "")
  local scope_kind = tostring(identity.scope_kind or "")

  -- Section 2: state
  local state      = packet.state or {}
  local status_v   = tostring(state.status or "")
  local next_action_v = tostring(state.next_action or "")

  -- Section 8: active_claim worktree path (if held).
  local active_claim = packet.active_claim  -- may be nil
  local worktree_path = nil
  if active_claim ~= nil then
    local wtp = active_claim.worktree_path
    if wtp ~= nil and tostring(wtp) ~= "" then
      worktree_path = tostring(wtp)
    end
  end

  flow.log("resume: task=" .. task_id ..
           " status=" .. status_v ..
           " next_action='" .. next_action_v .. "'")

  -- Build result. Omit nil/absent fields (plan_id when no plan,
  -- worktree_path when no active claim) — nil fields are not serialized.
  local result = {
    task_id     = task_id_v,
    title       = title_v,
    status      = status_v,
    next_action = next_action_v,
    scope_kind  = scope_kind,
  }
  if plan_id_v ~= nil then
    result.plan_id = plan_id_v
  end
  if worktree_path ~= nil then
    result.worktree_path = worktree_path
  end

  flow.log("resume: done")
  flow.result(result)
end
