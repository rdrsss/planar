--[[
  quality-spine.lua — doctrine-compliant cycle driver.

  TEMPLATE: This workflow demonstrates the per-cycle coder → reviewer cadence
  mandated by agents/methodology.md. It is NOT auto-installed; operators copy
  it to their own workflow dir and extend it with their brief-composition logic
  and worktree/claim pre-preparation before running it.

  Usage (after extending with real briefs):
    planar-execute run --mock-worker --plan <plan_id> workflows/quality-spine.lua <plan_id>
    planar-execute run --plan <plan_id> workflows/quality-spine.lua <plan_id>   # live

  meta.reviewer = true (line below) is the bright-line refusal guard assertion:
  it declares that this workflow dispatches a reviewer for every coder cycle.
  Without it, the guard would refuse to run a plan that touches migrations/*.sql,
  new top-level CLI verbs, or invariant/methodology code.

  DEFERRED HOST FUNCTIONS:
    Phase 3.5 (test-coder gated on spec ingest coverage) and Phase 6 (documenter)
    require host functions the harness does not yet expose:
      - ctx.test_spec_status(plan_id)  -- would shell `planar test-spec status`
                                       -- NOT available: `planar` is not on the
                                       -- constrained worker PATH.
      - ctx.documenter()               -- would shell `planar-doc diff`
                                       -- NOT available: same PATH constraint.
    When those host functions land, extend this workflow to call them after the
    coder/reviewer per-cycle pair. See the DEFERRED section near the bottom.

  OPERATOR EXTENSION POINTS:
    1. Replace "Coder brief for task …" with your actual brief-generation logic
       (call a helper, read a file, compose from ctx.args, etc.).
    2. Replace "Reviewer brief for coder result …" similarly.
    3. Provide worktree_path and claim_token via ctx.args or pre-prepare them
       before the workflow runs; pass them as extra ctx.args after the plan_id.
    4. Optionally call ctx.eligible(plan_id) to read the recommend-strategy
       output and decide which tasks to dispatch (this workflow does so already).
]]

return {
  meta = {
    name = "quality-spine",
    description = "Doctrine-compliant cycle driver: per-task coder → reviewer cadence. " ..
                  "TEMPLATE — extend with real brief-composition logic before running live.",
    -- Trust-based reviewer-cadence assertion for the bright-line refusal guard
    -- Setting this to `true` asserts that every task cycle in this workflow
    -- dispatches a reviewer after the coder. Do NOT set this to `true` in a
    -- copy of the template until you have wired the reviewer agent() call
    -- (see the "Cycle" phase below).
    reviewer = true,
    phases = {
      { title = "Plan",  detail = "Read plan eligibility via ctx.eligible(plan_id)" },
      { title = "Cycle", detail = "Per-eligible-task: coder agent → reviewer agent" },
      -- DEFERRED:
      -- { title = "Phase-3.5", detail = "Test-coder gated on coverage (ctx.test_spec_status)" },
      -- { title = "Phase-6",   detail = "Documenter (ctx.documenter)" },
    },
  },

  run = function(ctx)
    -- ---- Phase: Plan -------------------------------------------------------
    ctx.phase("Plan")

    local plan_id = tonumber(ctx.args[1])
      or error("usage: quality-spine.lua <plan-id> [task-args...]")

    -- ctx.eligible(plan_id) shells `planar plan recommend-strategy` against the
    -- fixture DB and returns:
    --   { eligible = [{id, slug, ...}], fan_out_available = bool,
    --     serialized = [{id, slug, excluded_by = [{rule, ...}]}] }
    local elig = ctx.eligible(plan_id)
    -- print() writes to stdout so the operator (and tests) can see progress.
    -- ctx.log() records into the host-side call journal (internal recording).
    print(string.format(
      "eligible: %d task(s)  serialized: %d task(s)  fan-out-available: %s",
      #elig.eligible, #elig.serialized, tostring(elig.fan_out_available)
    ))
    ctx.log(string.format(
      "eligible: %d  serialized: %d",
      #elig.eligible, #elig.serialized
    ))

    if #elig.eligible == 0 then
      print("no eligible tasks — nothing to dispatch")
      return
    end

    -- ---- Phase: Cycle (per-eligible-task coder → reviewer) -----------------
    ctx.phase("Cycle")

    for i, task in ipairs(elig.eligible) do
      local task_label = string.format("task:%d (%s)", task.id, task.slug or "")
      print(string.format("[%d/%d] %s — dispatching coder", i, #elig.eligible, task_label))

      -- OPERATOR: Replace the string literal below with your brief-generation
      -- logic. The brief is the full context the coder agent receives.
      -- Typically you read the task body via `planar task show <id>` output
      -- (passed through ctx.args or pre-composed before the workflow runs),
      -- the relevant spec sections, the claim token, and the worktree path.
      local coder_brief = string.format(
        "Coder brief for %s: <OPERATOR-FILLS-IN — replace this placeholder " ..
        "with the actual coder brief including spec citations, claim token, " ..
        "and worktree path>",
        task_label
      )

      -- OPERATOR: Pass worktree_path and claim_token. These are pre-prepared
      -- by the orchestrator before dispatching the workflow. Pass them via
      -- ctx.args or hard-code them in the extended copy of this template.
      -- worktree_path is required by the agent() host function; provide the
      -- real pre-prepared path here. The placeholder "/tmp/planar-wt" is used
      -- for --mock-worker runs (the FakeSpawner records but does not use it).
      local coder_r = ctx.agent(coder_brief, {
        role          = "coder",
        task_id       = task.id,
        task_slug     = task.slug,
        worktree_path = ctx.args[2] or "/tmp/planar-wt",
        -- claim_token = "<planar-agent pull / claim token>",
      })

      print(string.format(
        "[%d/%d] %s — coder: status=%s  commit=%s",
        i, #elig.eligible, task_label,
        coder_r.status,
        tostring(coder_r.commit_present)
      ))

      -- Dispatch the reviewer only when the coder produced something to review.
      -- "completed" means the claim terminal verb was `complete`; "released"
      -- means the coder surfaced no commit but the claim was released cleanly
      -- (barrel-bypass or early-exit). Both warrant a reviewer pass.
      if coder_r.status == "completed" or coder_r.status == "released" then
        print(string.format("[%d/%d] %s — dispatching reviewer", i, #elig.eligible, task_label))

        -- OPERATOR: Compose the reviewer brief from the coder's output. Include
        -- the coder's work-complete report, the original task body, and any
        -- files changed (typically read from the coder's commit or its stdout).
        local reviewer_brief = string.format(
          "Reviewer brief for coder result on %s: <OPERATOR-FILLS-IN — " ..
          "include coder work-complete report, files changed, and task " ..
          "acceptance criteria>",
          task_label
        )

        local reviewer_r = ctx.agent(reviewer_brief, {
          role          = "reviewer",
          task_id       = task.id,
          task_slug     = task.slug,
          worktree_path = ctx.args[2] or "/tmp/planar-wt",
        })

        print(string.format(
          "[%d/%d] %s — reviewer: status=%s",
          i, #elig.eligible, task_label,
          reviewer_r.status
        ))

        -- On request-changes (reviewer declined), the coder iterates.
        -- Iteration logic (re-dispatch with the reviewer's findings, up to the
        -- iteration cap) is an operator extension: copy this template and wrap
        -- the coder/reviewer pair in a while loop bounded by a max-iter counter.
        -- The iteration cap is enforced by the methodology; the workflow author
        -- is responsible for honouring it here.
        if reviewer_r.status ~= "completed" then
          ctx.log(string.format(
            "[%d/%d] %s — reviewer did not complete (status=%s); " ..
            "iteration logic is an OPERATOR extension of this template",
            i, #elig.eligible, task_label,
            reviewer_r.status
          ))
        end

      else
        -- Coder did not produce a reviewable result. Log and continue to the
        -- next task. Typical causes: coder was blocked (status="blocked"), the
        -- claim expired, or the spawn failed outright.
        ctx.log(string.format(
          "[%d/%d] %s — reviewer SKIPPED (coder status=%s)",
          i, #elig.eligible, task_label,
          coder_r.status
        ))
      end

    end -- ipairs(elig.eligible)

    -- ---- DEFERRED: Phase 3.5 (test-coder) + Phase 6 (documenter) ----------
    --
    -- These phases require host functions that the harness does not yet expose:
    --
    --   ctx.phase("Phase-3.5: test-coder")
    --   local coverage = ctx.test_spec_status(plan_id)
    --   -- ctx.test_spec_status(plan_id) would shell `planar test-spec status`
    --   -- but `planar` is NOT on the constrained worker PATH. When the host
    --   -- function lands, wire it here to gate a test-coder dispatch on the
    --   -- ingestor coverage report.
    --   if coverage.uncovered_count > 0 then
    --     ctx.agent("test-coder brief: ...", { role = "test-coder" })
    --   end
    --
    --   ctx.phase("Phase-6: documenter")
    --   -- ctx.documenter() would shell `planar-doc diff` — same PATH constraint.
    --   -- ctx.agent("documenter brief: ...", { role = "documenter" })
    --
    -- File project-local follow-up work when you add the missing host functions.
    -- -----------------------------------------------------------------------

    print("quality-spine cycle complete")
  end,
}
