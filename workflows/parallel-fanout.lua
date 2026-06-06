--[[
  parallel-fanout.lua — parallel N-way fanout driver (plan 492 M10 task 3205).

  TEMPLATE: Demonstrates `ctx.parallel({thunks})` to fan out coders across all
  parallel-eligible tasks simultaneously. Uses ctx.eligible(plan_id) to read
  the recommend-strategy output; only the eligible (disjoint-touch) tasks fan
  out. Serialized (overlapping-touch) tasks are logged as skipped.

  meta.reviewer = true: each thunk in the parallel block dispatches its own
  reviewer after the coder, satisfying the bright-line refusal guard (task 3206)
  for every branch of the fan-out.

  OPERATOR EXTENSION POINTS:
    1. Replace brief placeholder strings with real brief-generation logic.
    2. Provide worktree_path and claim_token per-task (M5 pre-preparation).
    3. Decide how to handle the serialized bucket: either ignore them (let the
       next run pick them up) or add a sequential loop after the parallel block.
]]

return {
  meta = {
    name = "parallel-fanout",
    description = "Fan out N coders in parallel across eligible tasks, each with " ..
                  "a per-coder reviewer. TEMPLATE — extend before running live.",
    reviewer = true,
    phases = {
      { title = "Plan",     detail = "ctx.eligible(plan_id) — read recommend-strategy" },
      { title = "Dispatch", detail = "ctx.parallel — N coders + N reviewers simultaneously" },
    },
  },

  run = function(ctx)
    ctx.phase("Plan")

    local plan_id = tonumber(ctx.args[1])
      or error("usage: parallel-fanout.lua <plan-id>")

    local elig = ctx.eligible(plan_id)
    -- print() writes to stdout for operator visibility and test assertions.
    print(string.format(
      "eligible: %d  serialized: %d  fan-out-available: %s",
      #elig.eligible, #elig.serialized, tostring(elig.fan_out_available)
    ))
    ctx.log(string.format(
      "eligible: %d  serialized: %d",
      #elig.eligible, #elig.serialized
    ))

    if #elig.eligible == 0 then
      print("no eligible tasks — nothing to fan out")
      return
    end

    if not elig.fan_out_available then
      -- Fewer than 2 parallel-eligible tasks; serial dispatch is more appropriate.
      -- The quality-spine.lua template handles the single-task case cleanly.
      print("fan-out NOT available (< 2 eligible tasks) — use quality-spine.lua instead")
      return
    end

    ctx.phase("Dispatch")
    print(string.format("fanning out %d coders in parallel", #elig.eligible))

    -- Build the thunk list: each thunk runs one coder + one reviewer.
    local thunks = {}
    for i, task in ipairs(elig.eligible) do
      local task_label = string.format("task:%d (%s)", task.id, task.slug or "")
      -- Capture loop variables by value in the closure.
      local captured_i = i
      local captured_task = task
      local captured_label = task_label
      thunks[i] = function()
        ctx.log(string.format("[branch %d] %s — coder start", captured_i, captured_label))

        -- OPERATOR: Replace with real brief-generation logic.
        local coder_brief = string.format(
          "Coder brief for %s: <OPERATOR-FILLS-IN>", captured_label
        )
        local coder_r = ctx.agent(coder_brief, {
          role          = "coder",
          task_id       = captured_task.id,
          task_slug     = captured_task.slug,
          worktree_path = ctx.args[2] or "/tmp/planar-wt",
        })

        ctx.log(string.format(
          "[branch %d] %s — coder: status=%s",
          captured_i, captured_label, coder_r.status
        ))

        if coder_r.status == "completed" or coder_r.status == "released" then
          -- OPERATOR: Replace with real reviewer brief.
          local reviewer_brief = string.format(
            "Reviewer brief for coder result on %s: <OPERATOR-FILLS-IN>",
            captured_label
          )
          local reviewer_r = ctx.agent(reviewer_brief, {
            role          = "reviewer",
            task_id       = captured_task.id,
            task_slug     = captured_task.slug,
            worktree_path = ctx.args[2] or "/tmp/planar-wt",
          })
          ctx.log(string.format(
            "[branch %d] %s — reviewer: status=%s",
            captured_i, captured_label, reviewer_r.status
          ))
          return reviewer_r.status
        else
          ctx.log(string.format(
            "[branch %d] %s — reviewer SKIPPED (coder=%s)",
            captured_i, captured_label, coder_r.status
          ))
          return "skipped"
        end
      end
    end

    -- ctx.parallel runs all thunks concurrently and returns a results table in
    -- the same order as the input thunks (original-order guarantee from M5).
    local results = ctx.parallel(thunks)

    -- Print the per-branch outcomes to stdout.
    for j, status in ipairs(results) do
      print(string.format("branch %d outcome: %s", j, tostring(status)))
    end

    -- Log serialized tasks that were not dispatched.
    if #elig.serialized > 0 then
      print(string.format(
        "%d serialized task(s) were NOT dispatched (overlapping touches — " ..
        "re-run the workflow after the parallel batch merges to pick them up):",
        #elig.serialized
      ))
      for _, st in ipairs(elig.serialized) do
        print(string.format("  serialized: task:%d (%s)", st.id, st.slug or ""))
      end
    end

    print("parallel-fanout complete")
  end,
}
