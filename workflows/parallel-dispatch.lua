--[[ @meta
name: parallel-dispatch
description: Deterministic, spawn-free wave/lane computation for the model orchestrator's parallel fan-out path. Computes the current wave (from recommend-strategy eligibility), the FULL ordered wave list (from recommend-strategy eligibility + the plan's `blocks` graph, for ordering/labeling only), each lane's worktree path + branch name, the epic branch name, the wave-barrier gate, and the stable fan-in merge order — then HANDS BACK. The model creates the worktrees, spawns the coders, and runs the git merges; this seam never spawns and never touches git worktree/branch/merge.
phases: plan, waves, barrier_check, fan_in
seam: planar plan recommend-strategy --json (via ctx.recommend_strategy), planar plan show --json (via ctx.plan_show), planar links list task:<id> --json (via cli.planar, for `blocks`-edge ordering only)
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
--   plan          — read recommend-strategy eligibility, take the
--                   currently-eligible tasks as the current wave
--                   (pairwise-disjoint by construction of the six eligibility
--                   rules — decision 370), compute each lane's worktree path +
--                   branch name and the epic branch name, and emit the wave
--                   plan. Fewer than two eligible tasks ⇒ NO fan-out wave (empty
--                   lanes, fan_out=false); the caller falls back to the in-pwd
--                   path. HANDS BACK: the model creates worktrees + spawns N
--                   coders. This is the CURRENT wave only (M1).
--
--   waves         — (M2, wave-ordering) emit the FULL ordered wave list for the
--                   plan by combining recommend-strategy eligibility with the
--                   plan's `blocks` graph. Waves are NOT depth-partitioned to
--                   gate eligibility — the engine's recommend-strategy stays the
--                   single eligibility gate. The `blocks` graph is walked ONLY
--                   to ORDER and LABEL the waves so the operator sees "wave 2
--                   unblocks once proto lands." Wave 1 is exactly the currently
--                   -eligible set; each later wave is the tasks whose remaining
--                   blockers are all in earlier waves, with the gating blockers
--                   named. A strict blocks-chain collapses to sequential
--                   one-lane waves. Serialized tasks with a NON-blocks exclusion
--                   are surfaced (with excluded_by) but never placed in a wave.
--                   HANDS BACK; the caller confirms the plan at the gate then
--                   drives one barrier at a time via `plan` + `fan_in`.
--
--   barrier_check — (M2, wave-barrier) given the state of the just-run wave's
--                   lanes, decide whether the NEXT wave may start. A lane
--                   satisfies the barrier only when it is BOTH landed AND
--                   fanned in; a lane whose coder completed but whose fan-in has
--                   NOT run does NOT satisfy the barrier. Emits proceed=true only
--                   when every lane is landed+fanned_in, else proceed=false with
--                   the blocking lanes named. Pure computation; the model
--                   enforces the gate. HANDS BACK.
--
--   fan_in        — given the landed lanes, emit the stable fan-in merge order
--                   (stable by task id) and the per-lane worktree paths to tear
--                   down. Pure computation; the model runs the actual
--                   `git merge --no-ff` and `git worktree remove`. HANDS BACK.
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
-- ## Eligibility is the engine's; the `blocks` graph is for ordering only (M2)
--
-- The `waves` phase reads the plan's `blocks` edges (via `planar links list
-- task:<id> --json`) but it NEVER decides eligibility with them. The engine's
-- recommend-strategy is the single eligibility gate (the six rules stay
-- authoritative). The `blocks` graph is used ONLY to compute the wave DEPTH of
-- each not-done task — i.e. to ORDER the waves and to LABEL which downstream
-- lanes are still blocked and by what. A task the engine serialized for a
-- NON-blocks reason (migration / singleton / empty-touches / open-question /
-- proposed-decision) is carried in `serialized` and never placed in a wave,
-- regardless of its blocks-depth. Because waves EMERGE by iterative recompute
-- across barriers (each barrier re-runs recommend-strategy and downstream lanes
-- become eligible only once their blockers are `done`), the `waves` phase is a
-- PROJECTION for the gate, not a schedule the seam executes: the model drives
-- one barrier at a time with `plan` (current wave) + `barrier_check` + `fan_in`.
--
-- ## --args contracts
--
--   plan:   { plan_id (int, required),
--             base (str, optional; epic-branch base ref, default "HEAD"),
--             worktree_root (str, optional; default ".worktrees") }
--
--   waves:  { plan_id (int, required),
--             base (str, optional; default "HEAD"),
--             worktree_root (str, optional; default ".worktrees") }
--
--   barrier_check: { plan_id (int, required),
--             lanes (array, required; each { task_id (int),
--                    landed (bool), fanned_in (bool) }) }
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
-- `blocks`-graph helpers (M2 — ORDERING/LABELING ONLY, never eligibility)
-- ---------------------------------------------------------------------------

--- blockers_of(task_id, in_plan) — the set of task ids that `task_id` is
-- blocked BY, restricted to tasks that are in this plan's not-done set.
--
-- Reads `planar links list task:<id> --json` (NDJSON, one flat object per
-- line). An edge `from_id=task_id, relationship="blocks", to_id=B` means
-- task_id is blocked BY B (see strategy.zig: task -[blocks]-> task, from_id is
-- blocked BY to_id). We extract the edges via line-wise Lua string patterns —
-- the row shape is flat and stable — because the confined host surface exposes
-- no Lua JSON decoder and `cli.planar_json` cannot parse NDJSON (multiple
-- top-level values). We keep only blockers that are members of `in_plan`
-- (a set keyed by task id) so cross-plan/stale edges do not perturb ordering.
local function blockers_of(task_id, in_plan)
  local raw = cli.planar({ "links", "list", "task:" .. tostr(task_id), "--json" })
  local out = {}
  if type(raw) ~= "string" then
    return out
  end
  -- Iterate line by line; each line is one entity_links JSON object.
  for line in (raw .. "\n"):gmatch("([^\n]*)\n") do
    if line ~= "" then
      local from_id = line:match('"from_id":(%d+)')
      local to_id = line:match('"to_id":(%d+)')
      local rel = line:match('"relationship":"([%w%-]+)"')
      if from_id ~= nil and to_id ~= nil and rel == "blocks" then
        local f = tonumber(from_id)
        local t = tonumber(to_id)
        -- Only THIS task's outgoing blocks edges (from_id == task_id), and
        -- only blockers that are in-plan not-done tasks.
        if f == task_id and in_plan[t] then
          out[#out + 1] = t
        end
      end
    end
  end
  return out
end

--- compute_depths(task_ids, blockers) — assign each task a wave DEPTH from the
-- `blocks` graph. depth = 0 for a task with no in-plan blocker; otherwise
-- depth = 1 + max(depth(blocker)). Deterministic longest-path over the DAG.
--
-- `blockers` is a map task_id -> array of in-plan blocker task ids (from
-- blockers_of). A cycle (which the engine would have serialized under rule 1
-- anyway) is broken defensively: a task still unresolved after N passes is
-- pinned to the current max depth so the computation always terminates and is
-- byte-stable. Returns a map task_id -> depth.
local function compute_depths(task_ids, blockers)
  local depth = {}
  for _, id in ipairs(task_ids) do
    depth[id] = nil
  end
  -- Iteratively resolve: a task's depth is known once all its blockers'
  -- depths are known. Bounded by #task_ids passes (longest chain length).
  local n = #task_ids
  for _ = 1, n do
    local progressed = false
    for _, id in ipairs(task_ids) do
      if depth[id] == nil then
        local bs = blockers[id] or {}
        local max_blocker = -1
        local all_known = true
        for _, b in ipairs(bs) do
          if depth[b] == nil then
            all_known = false
            break
          end
          if depth[b] > max_blocker then
            max_blocker = depth[b]
          end
        end
        if all_known then
          depth[id] = max_blocker + 1
          progressed = true
        end
      end
    end
    if not progressed then
      break
    end
  end
  -- Defensive cycle-break: pin any still-unresolved task to depth 0 so the
  -- result is total and deterministic. The engine serializes true cycles
  -- (rule 1) so this is belt-and-suspenders, never the happy path.
  for _, id in ipairs(task_ids) do
    if depth[id] == nil then
      depth[id] = 0
    end
  end
  return depth
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
-- Phase: waves (M2 — wave-ordering)
--
-- Emit the FULL ordered wave list for the plan. Eligibility gating stays the
-- engine's: recommend-strategy's `parallel_eligible` IS wave 1's membership
-- candidate pool. The `blocks` graph is walked ONLY to ORDER later waves and to
-- LABEL which tasks are still blocked and by what.
--
-- Algorithm:
--   1. recommend-strategy → the plan's not-done tasks split into eligible-now
--      and serialized (each serialized task carries its excluded_by rules).
--   2. For ORDERING, build the set of tasks that are candidates for SOME wave:
--      the eligible-now tasks PLUS the tasks serialized ONLY by rule 1
--      (blocked_by a not-done task). A rule-1-only serialization means the task
--      is fine to fan out once its blocker lands — it belongs in a LATER wave.
--      A task serialized by ANY non-blocks rule is NOT wave-eligible ever; it
--      is carried in `serialized` and never placed in a wave.
--   3. Read each candidate's in-plan `blocks` blockers and compute wave depth
--      (0 = unblocked now; k = one past its deepest blocker). Group candidates
--      by depth → ordered waves. A strict A→B→C chain yields three one-lane
--      waves; a fully-parallel set yields one wave.
--   4. For each wave, compute per-lane { task_id, slug, branch, worktree } and,
--      for later waves, the `blocked_by` blocker task ids that gate the lane.
--   5. Emit ordered `waves` + the epic branch + the never-in-a-wave serialized
--      set. HAND BACK: the operator confirms at the gate; the model then drives
--      ONE barrier at a time (recompute via `plan`, gate via `barrier_check`,
--      merge via `fan_in`) — this list is the projection, not a schedule the
--      seam executes.
-- ---------------------------------------------------------------------------
function waves()
  flow.phase("waves")
  flow.log("parallel-dispatch.lua/waves: starting")

  local plan_id = require_arg("plan_id")
  local base = opt_arg("base", "HEAD")
  local worktree_root = opt_arg("worktree_root", ".worktrees")

  -- 1. Authoritative eligibility. Never re-derived here.
  local rec = ctx.recommend_strategy(plan_id)
  if type(rec) ~= "table" then
    flow.fail("parallel-dispatch.lua/waves: recommend-strategy returned no table for plan " .. tostr(plan_id))
    return
  end
  local eligible = rec.parallel_eligible
  if type(eligible) ~= "table" then
    eligible = {}
  end
  local rec_serialized = rec.serialized
  if type(rec_serialized) ~= "table" then
    rec_serialized = {}
  end

  local plan_info = ctx.plan_show(plan_id)
  local pslug = plan_slug(plan_info, plan_id)
  local epic = epic_branch(plan_id, pslug)

  -- 2. Partition the not-done set into wave-candidates vs never-in-a-wave
  --    serialized tasks. A task serialized ONLY by rule 1 (blocked_by) is a
  --    LATER-wave candidate; any other serialization keeps it out of all waves.
  local candidates = {} -- array of { id, slug, title }
  local task_meta = {} -- id -> { slug, title }
  local in_plan = {} -- id -> true (candidate membership, for blocks filtering)
  local serialized = {} -- never-in-a-wave, carried through untouched

  for _, t in ipairs(eligible) do
    candidates[#candidates + 1] = { id = t.id, slug = t.slug, title = t.title }
    task_meta[t.id] = { slug = t.slug, title = t.title }
    in_plan[t.id] = true
  end

  for _, t in ipairs(rec_serialized) do
    -- Determine whether this task is serialized SOLELY by rule 1 (blocks).
    local only_blocks = true
    local reasons = {}
    if type(t.excluded_by) == "table" then
      for _, e in ipairs(t.excluded_by) do
        reasons[#reasons + 1] = { rule = e.rule, reason = e.reason }
        if e.rule ~= 1 then
          only_blocks = false
        end
      end
    else
      only_blocks = false
    end
    if only_blocks and #reasons > 0 then
      -- Later-wave candidate: eligible once its blocker(s) land.
      candidates[#candidates + 1] = { id = t.id, slug = t.slug, title = t.title }
      task_meta[t.id] = { slug = t.slug, title = t.title }
      in_plan[t.id] = true
    else
      -- Never in a wave: carry through with excluded_by.
      serialized[#serialized + 1] = {
        task_id = t.id,
        slug = t.slug,
        title = t.title,
        excluded_by = reasons,
      }
    end
  end

  -- 3. Read `blocks` blockers for each candidate (in-plan only) and compute
  --    wave depth over the candidate DAG.
  local candidate_ids = {}
  for _, c in ipairs(candidates) do
    candidate_ids[#candidate_ids + 1] = c.id
  end
  table.sort(candidate_ids)

  local blockers = {}
  for _, id in ipairs(candidate_ids) do
    blockers[id] = blockers_of(id, in_plan)
  end
  local depth = compute_depths(candidate_ids, blockers)

  -- 4. Group candidates by depth into ordered waves (1-based wave numbers).
  local max_depth = 0
  for _, id in ipairs(candidate_ids) do
    if depth[id] > max_depth then
      max_depth = depth[id]
    end
  end

  local wave_list = {}
  for d = 0, max_depth do
    local lanes = {}
    for _, id in ipairs(candidate_ids) do
      if depth[id] == d then
        local m = task_meta[id]
        local tslug = task_slug({ id = id, slug = m.slug })
        -- Which of this task's blockers gate it (named for the gate label).
        local blocked_by = {}
        for _, b in ipairs(blockers[id]) do
          blocked_by[#blocked_by + 1] = b
        end
        table.sort(blocked_by)
        lanes[#lanes + 1] = {
          task_id = id,
          slug = tslug,
          title = m.title,
          branch = lane_branch(plan_id, tslug),
          worktree = lane_worktree(worktree_root, plan_id, tslug),
          blocked_by = blocked_by,
        }
      end
    end
    if #lanes > 0 then
      sort_by_task_id(lanes)
      wave_list[#wave_list + 1] = {
        wave = d + 1,
        lane_count = #lanes,
        lanes = lanes,
      }
    end
  end

  flow.log("parallel-dispatch.lua/waves: computed " .. tostr(#wave_list)
    .. " wave(s) on epic " .. epic)
  flow.result({
    plan_id = plan_id,
    epic_branch = epic,
    base = base,
    worktree_root = worktree_root,
    wave_count = #wave_list,
    waves = wave_list,
    serialized = serialized,
  })
end

-- ---------------------------------------------------------------------------
-- Phase: barrier_check (M2 — wave-barrier)
--
-- Given the state of the just-run wave's lanes, decide whether the NEXT wave
-- may start. The barrier is on FAN-IN completion, not coder completion: a lane
-- satisfies the barrier only when it is BOTH landed AND fanned in. A lane whose
-- coder reported done (landed=true) but whose branch has not been merged into
-- the epic (fanned_in=false) does NOT satisfy the barrier.
--
-- Emits { proceed, blocking } — proceed=true iff every lane is landed AND
-- fanned_in; otherwise proceed=false and `blocking` names each lane that holds
-- the barrier with the reason (not_landed | not_fanned_in). Pure computation;
-- the model enforces the gate (does not create the next wave's worktrees while
-- proceed=false). HAND BACK.
-- ---------------------------------------------------------------------------
function barrier_check()
  flow.phase("barrier_check")
  flow.log("parallel-dispatch.lua/barrier_check: starting")

  local plan_id = require_arg("plan_id")
  local in_lanes = require_arg("lanes")
  if type(in_lanes) ~= "table" then
    flow.fail("parallel-dispatch.lua/barrier_check: lanes must be an array")
    return
  end

  local blocking = {}
  for _, l in ipairs(in_lanes) do
    if l.task_id == nil then
      flow.fail("parallel-dispatch.lua/barrier_check: each lane requires task_id")
      return
    end
    local landed = l.landed == true
    local fanned_in = l.fanned_in == true
    -- A lane satisfies the barrier only when landed AND fanned_in. Report the
    -- specific reason so the operator sees WHICH gate a lane is stuck at.
    if not landed then
      blocking[#blocking + 1] = { task_id = l.task_id, reason = "not_landed" }
    elseif not fanned_in then
      blocking[#blocking + 1] = { task_id = l.task_id, reason = "not_fanned_in" }
    end
  end
  sort_by_task_id(blocking)

  local proceed = #blocking == 0
  flow.log("parallel-dispatch.lua/barrier_check: proceed=" .. tostr(proceed)
    .. " (" .. tostr(#blocking) .. " lane(s) holding the barrier)")
  flow.result({
    plan_id = plan_id,
    proceed = proceed,
    blocking = blocking,
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
