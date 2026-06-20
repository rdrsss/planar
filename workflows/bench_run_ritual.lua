--[[ @meta
name: bench-run-ritual
description: Deterministic clean-slate measurement run ritual — reset worktree, bench start, harvest, bench finish.
phases: setup, harvest
seam: planar run start/event/finish, planar bench start/harvest/finish, git reset/clean
--]]

-- bench_run_ritual.lua — the deterministic clean-slate run ritual as a
-- planar-execute workflow (plan 635, M1.4).
--
-- This workflow drives the measurement-rig run ritual described in
-- docs/research/run-record-schema.md §1 and closure-measurement-build-spec.md
-- §5 M1 task 4:
--
--     reset to base_sha  →  bench start  →  (LLM coder/reviewer step, OUTSIDE
--     this engine, driven by the caller)  →  harvest  →  bench finish
--
-- ## The A/C phase split (decision D7 — one clean process per --phase)
--
-- planar-execute runs ONE deterministic phase per process and hands control
-- back to the caller. The non-deterministic LLM coder/reviewer step happens
-- BETWEEN the engine phases, in the caller's loop — never inside this file.
-- So the ritual is split into two deterministic phases:
--
--   * setup   (phase A) — clean the worktree to base_sha and open the run.
--                         `bench start` snapshots the plan's declared touches
--                         into the run in-transaction (M1.5); this phase no
--                         longer writes them explicitly. Exits; the caller
--                         then runs the LLM coder slice.
--   * measure (phase C) — after the (simulated or real) edit, harvest the
--                         actual git diff per task and finish the run.
--
-- (There is deliberately no phase B in this file: phase B is the caller's
-- LLM step, which planar-execute has NO primitive to perform — D5/D7.)
--
-- ## Spawn-free by construction
--
-- This workflow holds NO database handle and imports nothing. It reaches all
-- state by shelling `bench *` via cli.planar (the protected-instrument
-- invariant: the harness records runs the same way it reads state — by
-- subprocess). The host functions used are exactly the D7 surface:
--   cli.planar, cli.planar_json, git.reset_hard, ctx.args, flow.{phase,log,
--   result,fail}.
--
-- ## How to run
--
--   planar-execute run workflows/bench_run_ritual.lua \
--       --phase setup --worktree <dir> --args '<json>'
--   # ... caller performs the LLM coder/reviewer step in <dir> ...
--   planar-execute run workflows/bench_run_ritual.lua \
--       --phase measure --worktree <dir> --args '<json>'
--
-- ## --args contract (ctx.args)
--
--   run_uid      (string, required)  harness-minted stable id (D8)
--   plan_id      (int,    required)  the plan under measurement
--   base_sha     (string, required for setup)  the clean-slate reset point
--   config_hash  (string, required for setup)  the cell GROUP BY key
--   arm          (string, optional, default "strict")  experimental arm
--   config_json  (string, optional)  opaque audit blob
--   corpus_repo  (string, optional)  corpus member name
--   tasks        (array of ints, required)  the task ids in the slice
--   status       (string, optional, default "completed")  terminal status

-- ---------------------------------------------------------------------------
-- small helpers
-- ---------------------------------------------------------------------------

-- require_arg(name) returns ctx.args[name] or fails the phase loudly.
local function require_arg(name)
  local v = ctx.args[name]
  if v == nil then
    flow.fail("bench_run_ritual: missing required --args field: " .. name)
  end
  return v
end

-- tostr(v) coerces a number/string arg to a string for the CLI argv.
local function tostr(v)
  return tostring(v)
end

-- task_id_list() reads ctx.args.tasks (a JSON array) into a Lua array of
-- numbers. Fails if absent or empty.
local function task_id_list()
  local tasks = require_arg("tasks")
  if type(tasks) ~= "table" or #tasks == 0 then
    flow.fail("bench_run_ritual: --args.tasks must be a non-empty array of task ids")
  end
  return tasks
end

-- ---------------------------------------------------------------------------
-- Phase A: setup
--
-- Deterministic clean-slate open. Resets the run's worktree to base_sha and
-- opens the run record. `bench start` itself snapshots the plan's declared
-- touches into the run, in the same transaction as the runs row insert.
--
-- THE DECLARED-SNAPSHOT SEAM (M1.5) IS NOW CLOSED:
--   As of M1.5 ("Declared-touch snapshot at bench start"), the engine reads
--   the plan's task_touch_paths and writes one run_touches(kind='declared')
--   row per declared (task, path) inside `bench start`'s transaction. The
--   ritual no longer snapshots them explicitly — the old per-(task,path)
--   `bench touch --kind declared` loop was DELETED (re-running it here would
--   double-write and trip the UNIQUE(run_id,task_id,path,kind) constraint).
--   The snapshot is an immutable value-copy: re-declaring touches afterward
--   cannot rewrite the recorded prediction (run-record-schema.md §2).
--   The ritual reads the resulting declared count back from `bench show`.
-- ---------------------------------------------------------------------------
function setup()
  flow.phase("setup")
  flow.log("bench_run_ritual: setup (phase A) starting")

  local run_uid     = require_arg("run_uid")
  local plan_id     = require_arg("plan_id")
  local base_sha    = require_arg("base_sha")
  local config_hash = require_arg("config_hash")
  local arm         = ctx.args.arm or "strict"
  -- tasks is still required (the measure phase harvests per-task), and asserts
  -- the slice is non-empty before we open a run.
  local _tasks      = task_id_list()

  -- 1. Clean-slate reset: every arm for a corpus member starts from this exact
  --    SHA in its worktree (run-record-schema §1). git.* is confined by the
  --    host to the --worktree dir; the script never names the path.
  flow.log("bench_run_ritual: git reset --hard " .. tostr(base_sha))
  git.reset_hard(tostr(base_sha))

  -- 2. Open the run record (which snapshots declared touches in-transaction).
  --    run_uid is the harness-minted positional (D8).
  local start_argv = {
    "bench", "start", tostr(run_uid),
    "--plan", tostr(plan_id),
    "--arm", tostr(arm),
    "--base-sha", tostr(base_sha),
    "--config-hash", tostr(config_hash),
  }
  if ctx.args.config_json ~= nil then
    start_argv[#start_argv + 1] = "--config-json"
    start_argv[#start_argv + 1] = tostr(ctx.args.config_json)
  end
  if ctx.args.corpus_repo ~= nil then
    start_argv[#start_argv + 1] = "--corpus-repo"
    start_argv[#start_argv + 1] = tostr(ctx.args.corpus_repo)
  end
  cli.planar(start_argv)

  -- 3. Read back the declared count the engine snapshotted at start. The
  --    declared rows are present in the run now without any explicit touch
  --    loop here (M1.5 closed that seam). cli.planar_json shells
  --    `planar bench show --json` and returns the parsed table.
  local run = cli.planar_json({ "bench", "show", tostr(run_uid), "--json" })
  local declared = 0
  for _, t in ipairs(run.touches or {}) do
    if t.kind == "declared" then
      declared = declared + 1
    end
  end
  flow.log("bench_run_ritual: bench start snapshotted " .. tostr(declared) .. " declared touch(es)")

  flow.result({
    ok            = true,
    phase         = "setup",
    run_uid       = run_uid,
    plan_id       = plan_id,
    arm           = arm,
    declared_touches = declared,
  })
end

-- ---------------------------------------------------------------------------
-- Phase C: measure
--
-- Deterministic fan-in. After the caller's LLM coder/reviewer step has made
-- its edits in the worktree, harvest the actual git diff per task (git diff
-- → run_touches kind=actual) and set the run's terminal status.
-- ---------------------------------------------------------------------------
function measure()
  flow.phase("measure")
  flow.log("bench_run_ritual: measure (phase C) starting")

  local run_uid  = require_arg("run_uid")
  local worktree = require_arg("worktree")
  local tasks    = task_id_list()
  local status   = ctx.args.status or "completed"

  -- 1. Harvest actual touches per task. `bench harvest` runs `git diff
  --    --name-only` against the worktree and writes kind=actual rows.
  --    The worktree path is passed explicitly because `bench harvest`
  --    (a `planar` verb, NOT a git.* host call) takes --worktree.
  local harvested = 0
  for _, task_id in ipairs(tasks) do
    local out = cli.planar({
      "bench", "harvest", tostr(run_uid),
      "--task", tostr(task_id),
      "--worktree", tostr(worktree),
    })
    -- `bench harvest` prints the count of actual rows written.
    local n = tonumber((out:gsub("%s+", ""))) or 0
    harvested = harvested + n
  end
  flow.log("bench_run_ritual: harvested " .. tostr(harvested) .. " actual touch(es)")

  -- 2. Finish the run with its terminal status.
  cli.planar({ "bench", "finish", tostr(run_uid), "--status", tostr(status) })

  flow.result({
    ok               = true,
    phase            = "measure",
    run_uid          = run_uid,
    status           = status,
    actual_touches   = harvested,
  })
end
