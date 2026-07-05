--[[ @meta
name: parallel-dispatch
description: Deterministic, spawn-free wave/lane computation for the model orchestrator's parallel fan-out path. Computes the current wave (from recommend-strategy eligibility), each lane's worktree path + branch name, the epic branch name, and the stable fan-in merge order — then HANDS BACK. The model creates the worktrees, spawns the coders, and runs the git merges; this seam never spawns and never touches git worktree/branch/merge.
phases: plan, fan_in
seam: planar plan recommend-strategy --json (via ctx.recommend_strategy), planar plan show --json (via ctx.plan_show)
--]]

-- parallel-dispatch.lua — deterministic single-wave fan-out planning (plan 760 M1).
--
-- This workflow implements the DETERMINISTIC computation for the orchestrator's
-- parallel fan-out path. It does NOT spawn coders and it does NOT create
-- worktrees, cut branches, or run merges — those are the model's job (worktree
-- git ops) and the harness Agent tool's job (spawning). The seam computes the
-- wave, the per-lane paths/branch-names, the epic branch name, and the merge
-- order, then hands the plan back as byte-stable JSON via flow.result.
--
-- ## Why a spawn-free seam (tech-spec §Decisions "Inline-model fan-out")
--
-- The spawn primitive is the harness Agent/Task tool; re-adding a model-spawning
-- host function is exactly the scope creep that turned the old planar-execute
-- into a harness and got it extracted. So the seam is the DETERMINISTIC part
-- only: wave computation, worktree/branch bookkeeping, merge-order computation.
-- The model executes the seam's output and spawns.
--
-- ## Phases
--
--   plan    — read recommend-strategy eligibility, take the currently-eligible
--             tasks as the current wave (pairwise-disjoint by construction of
--             the six eligibility rules — decision 370), compute each lane's
--             worktree path + branch name and the epic branch name, and emit the
--             wave plan. Fewer than two eligible tasks ⇒ NO fan-out wave (empty
--             lanes, fan_out=false); the caller falls back to the in-pwd path.
--             HANDS BACK: the model creates worktrees + spawns N coders.
--
--   fan_in  — given the landed lanes, emit the stable fan-in merge order (stable
--             by task id) and the per-lane worktree paths to tear down. Pure
--             computation; the model runs the actual `git merge --no-ff` and
--             `git worktree remove`. HANDS BACK.
--
-- ## Eligibility is the engine's, never the seam's
--
-- recommend-strategy is the SINGLE eligibility gate (tech-spec §Decisions "The
-- six eligibility rules stay authoritative"). The seam NEVER re-derives which
-- tasks are eligible; it takes `parallel_eligible` verbatim and carries the
-- `serialized` set (with each task's `excluded_by` reasons) through untouched.
-- A serialized task is never placed in a lane.
--
-- ## Byte-stable output (tech-spec §Open-Questions seam resolution)
--
-- A resume must recompute IDENTICAL waves, so the result must be byte-stable
-- across runs. Two things guarantee that: (a) lanes are emitted as an ordered
-- array sorted by task id (never a hash-ordered map), and (b) planar-execute's
-- JSON serializer emits object keys in sorted bytewise order (host.zig). Given
-- identical recommend-strategy output, the seam produces identical bytes.
--
-- ## Naming (tech-spec §Decisions "Lane branch / worktree naming")
--
--   epic branch   : epic/p<plan>-<plan-slug>
--   lane branch   : cycle/p<plan>/<task-slug>
--   lane worktree : <worktree_root>/cycle/p<plan>/<task-slug>
--
-- The task slug falls back to `task-<id>` when a task has no slug; the plan slug
-- falls back to `p<plan>` when the plan has no slug. These fallbacks are
-- deterministic (derived from the ids), preserving byte-stability.
--
-- ## --args contracts
--
--   plan:   { plan_id (int, required),
--             base (str, optional; epic-branch base ref, default "HEAD"),
--             worktree_root (str, optional; default ".worktrees") }
--
--   fan_in: { plan_id (int, required),
--             lanes (array, required; each { task_id (int), branch (str),
--                    worktree (str) } as emitted by `plan`),
--             worktree_root (str, optional; only used for defaults) }

-- ---------------------------------------------------------------------------
-- helpers
-- ---------------------------------------------------------------------------

--- require_arg(name) — fail loudly if ctx.args[name] is nil.
local function require_arg(name)
  local v = ctx.args[name]
  if v == nil then
    flow.fail("parallel-dispatch.lua: missing required ctx.args field: " .. name)
  end
  return v
end

--- tostr(v) — coerce to string for path/branch construction.
local function tostr(v)
  return tostring(v)
end

--- opt_arg(name, default) — ctx.args[name] or default.
local function opt_arg(name, default)
  local v = ctx.args[name]
  if v == nil then
    return default
  end
  return v
end

--- task_slug(task, plan_id) — the deterministic slug for a task's branch/path.
-- Falls back to `task-<id>` when the task carries no slug, so the naming is
-- always well-formed and byte-stable (derived from the integer id).
local function task_slug(task)
  if task.slug ~= nil and task.slug ~= "" then
    return task.slug
  end
  return "task-" .. tostr(task.id)
end

--- plan_slug(plan, plan_id) — the deterministic slug for the epic branch.
-- Falls back to `p<plan_id>` when the plan carries no slug.
local function plan_slug(plan, plan_id)
  if type(plan) == "table" and plan.slug ~= nil and plan.slug ~= "" then
    return plan.slug
  end
  return "p" .. tostr(plan_id)
end

--- epic_branch(plan_id, pslug) — epic/p<plan>-<plan-slug>.
local function epic_branch(plan_id, pslug)
  return "epic/p" .. tostr(plan_id) .. "-" .. pslug
end

--- lane_branch(plan_id, tslug) — cycle/p<plan>/<task-slug>.
local function lane_branch(plan_id, tslug)
  return "cycle/p" .. tostr(plan_id) .. "/" .. tslug
end

--- lane_worktree(worktree_root, plan_id, tslug) — <root>/cycle/p<plan>/<task-slug>.
local function lane_worktree(worktree_root, plan_id, tslug)
  return worktree_root .. "/cycle/p" .. tostr(plan_id) .. "/" .. tslug
end

--- sort_by_task_id(lanes) — in-place ascending sort by lane.task_id.
-- The stable, byte-reproducible lane order the whole seam depends on. Lua's
-- table.sort is not guaranteed stable, but task ids are unique per plan so the
-- comparator is a total order and the result is deterministic.
local function sort_by_task_id(lanes)
  table.sort(lanes, function(a, b)
    return a.task_id < b.task_id
  end)
end

-- ---------------------------------------------------------------------------
-- Phase: plan
--
-- 1. Read recommend-strategy --json for the plan (authoritative eligibility).
-- 2. The parallel_eligible set IS the current wave. Fewer than two eligible
--    tasks ⇒ NO fan-out wave (empty lanes, fan_out=false).
-- 3. For each eligible task compute { task_id, slug, branch, worktree }.
-- 4. Compute the epic branch name from the plan slug.
-- 5. Carry the serialized set (with excluded_by reasons) through untouched.
-- 6. Emit the wave plan sorted by task id (byte-stable). HAND BACK — the model
--    creates the epic branch + worktrees and spawns the coders.
-- ---------------------------------------------------------------------------
function plan()
  flow.phase("plan")
  flow.log("parallel-dispatch.lua/plan: starting")

  local plan_id = require_arg("plan_id")
  local base = opt_arg("base", "HEAD")
  local worktree_root = opt_arg("worktree_root", ".worktrees")

  -- 1. Authoritative eligibility. The six rules are NEVER re-derived here.
  flow.log("parallel-dispatch.lua/plan: reading recommend-strategy for plan " .. tostr(plan_id))
  local rec = ctx.recommend_strategy(plan_id)
  if type(rec) ~= "table" then
    flow.fail("parallel-dispatch.lua/plan: recommend-strategy returned no table for plan " .. tostr(plan_id))
    return
  end

  local eligible = rec.parallel_eligible
  if type(eligible) ~= "table" then
    eligible = {}
  end

  -- 2. Plan slug for the epic branch name (deterministic; ctx.plan_show is a
  --    deterministic read).
  local plan_info = ctx.plan_show(plan_id)
  local pslug = plan_slug(plan_info, plan_id)
  local epic = epic_branch(plan_id, pslug)

  -- 3. Carry the serialized set through untouched (id, slug, title, excluded_by).
  local serialized = {}
  if type(rec.serialized) == "table" then
    for _, t in ipairs(rec.serialized) do
      local reasons = {}
      if type(t.excluded_by) == "table" then
        for _, e in ipairs(t.excluded_by) do
          reasons[#reasons + 1] = { rule = e.rule, reason = e.reason }
        end
      end
      serialized[#serialized + 1] = {
        task_id = t.id,
        slug = t.slug,
        title = t.title,
        excluded_by = reasons,
      }
    end
  end

  -- 4. Single-wave rule (tech-spec §Components "Wave planner"): the eligible set
  --    IS the current wave. Fewer than two eligible tasks ⇒ NO fan-out wave.
  --    This is a legitimately empty result (the Empty-null scenario), not an
  --    error — the caller falls back to the in-pwd path.
  local eligible_count = #eligible
  if eligible_count < 2 then
    flow.log("parallel-dispatch.lua/plan: fan_out_available=false ("
      .. tostr(eligible_count) .. " eligible task(s)); emitting empty wave")
    flow.result({
      plan_id = plan_id,
      fan_out = false,
      reason = "fewer than two eligible tasks; run sequentially in-pwd",
      epic_branch = epic,
      base = base,
      lane_count = 0,
      lanes = {},
      serialized = serialized,
    })
    return
  end

  -- 5. Compute one lane per eligible task.
  local lanes = {}
  for _, t in ipairs(eligible) do
    local tslug = task_slug(t)
    lanes[#lanes + 1] = {
      task_id = t.id,
      slug = tslug,
      title = t.title,
      branch = lane_branch(plan_id, tslug),
      worktree = lane_worktree(worktree_root, plan_id, tslug),
    }
  end

  -- 6. Sort by task id for byte-stable output.
  sort_by_task_id(lanes)

  flow.log("parallel-dispatch.lua/plan: wave computed with " .. tostr(#lanes)
    .. " lanes on epic " .. epic)
  flow.result({
    plan_id = plan_id,
    fan_out = true,
    epic_branch = epic,
    base = base,
    worktree_root = worktree_root,
    lane_count = #lanes,
    lanes = lanes,
    serialized = serialized,
  })
end

-- ---------------------------------------------------------------------------
-- Phase: fan_in
--
-- Given the landed lanes, compute the stable fan-in merge order and the
-- per-lane worktrees to tear down. Pure computation — the model runs the actual
-- `git merge --no-ff <lane-branch>` into the epic branch (in this order) and
-- `git worktree remove <worktree>` for each succeeded lane. HAND BACK.
--
-- Merge order is stable by task id (M1 single wave; M3 generalizes to
-- contract-lanes-first across waves). Emitting the order deterministically here
-- keeps the model from having to re-derive it.
-- ---------------------------------------------------------------------------
function fan_in()
  flow.phase("fan_in")
  flow.log("parallel-dispatch.lua/fan_in: starting")

  local plan_id = require_arg("plan_id")
  local in_lanes = require_arg("lanes")
  if type(in_lanes) ~= "table" then
    flow.fail("parallel-dispatch.lua/fan_in: lanes must be an array")
    return
  end

  -- Normalize + validate each lane, then sort by task id for the stable
  -- merge order.
  local lanes = {}
  for _, l in ipairs(in_lanes) do
    if l.task_id == nil or l.branch == nil then
      flow.fail("parallel-dispatch.lua/fan_in: each lane requires task_id and branch")
      return
    end
    lanes[#lanes + 1] = {
      task_id = l.task_id,
      branch = l.branch,
      worktree = l.worktree,
    }
  end
  sort_by_task_id(lanes)

  -- Emit the merge order (branch list) and the teardown list (worktree paths),
  -- both in the same stable task-id order.
  local merge_order = {}
  local teardown = {}
  for _, l in ipairs(lanes) do
    merge_order[#merge_order + 1] = l.branch
    if l.worktree ~= nil then
      teardown[#teardown + 1] = l.worktree
    end
  end

  flow.log("parallel-dispatch.lua/fan_in: merge order computed for "
    .. tostr(#merge_order) .. " lanes")
  flow.result({
    plan_id = plan_id,
    lane_count = #lanes,
    merge_order = merge_order,
    teardown_worktrees = teardown,
  })
end
