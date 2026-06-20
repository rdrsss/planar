--[[ @meta
name: introspect
description: Usage-introspection mining pipeline — classifies failure/retry/abandoned/gap signals and files findings.
phases: introspect
seam: planar report --json, planar-watch, planar question add, planar task add, planar run start/event/finish
--]]

-- introspect.lua — usage-introspection mining pipeline (plan 638 tasks 4120, 4121).
--
-- Phase: introspect
-- Args:  { plan_id (feedback plan id), days? (default 30), scope? }
--
-- Reads signals from `planar report --json` and `planar-watch` read surfaces,
-- classifies them into the four closed taxonomy categories, deduplicates against
-- existing findings, and files new findings as questions/tasks on the feedback
-- plan.
--
-- Taxonomy (closed set — introspect.zig mirrors this classification):
--   failure-cluster    : verb_path with failure_count >= 3 in the window → question
--   retry-pattern      : verb_path invoked >= 3 times total with failures before
--                        a success (failure_count >= 3 and success_count >= 1) → question
--   abandoned-workflow : stale claims (claims.stale_claims > 0) or stale
--                        handoffs (handoffs.stale_handoffs > 0) → task
--   gap-feature        : verb_path failure_count >= 3 with error_category
--                        containing "unknown" or "not found" flags → question
--
-- Finding title format: "<taxonomy-key>: <signal-key>"
-- This is deterministic; the same signal always produces the same title.
-- Title-based dedup makes re-runs idempotent (task 4121).

-- ---------------------------------------------------------------------------
-- helpers
-- ---------------------------------------------------------------------------

--- trim whitespace from a string
local function trim(s)
  return (s or ""):match("^%s*(.-)%s*$")
end

--- split a newline-delimited string into a list of non-empty lines
local function lines(s)
  local out = {}
  for line in (s .. "\n"):gmatch("([^\n]*)\n") do
    local t = trim(line)
    if #t > 0 then out[#out + 1] = t end
  end
  return out
end

--- check whether a string contains a substring (case-insensitive)
local function contains_ci(haystack, needle)
  return (haystack:lower()):find(needle:lower(), 1, true) ~= nil
end

-- ---------------------------------------------------------------------------
-- Dedup helpers
-- ---------------------------------------------------------------------------

--- collect existing finding titles from the feedback plan.
--- returns a set table: {title → true}
local function collect_existing_titles(plan_id)
  local existing = {}

  -- questions with status open
  local ok_q, q_list = pcall(function()
    return cli.planar_json({"question", "list", "--plan", tostring(plan_id), "--status", "open", "--json"})
  end)
  if ok_q and type(q_list) == "table" then
    for _, q in ipairs(q_list) do
      if q.title then existing[q.title] = true end
    end
  end

  -- tasks with status todo
  local ok_todo, t_todo = pcall(function()
    return cli.planar_json({"task", "list", "--plan", tostring(plan_id), "--status", "todo", "--json"})
  end)
  if ok_todo and type(t_todo) == "table" then
    for _, t in ipairs(t_todo) do
      if t.title then existing[t.title] = true end
    end
  end

  -- tasks with status doing
  local ok_doing, t_doing = pcall(function()
    return cli.planar_json({"task", "list", "--plan", tostring(plan_id), "--status", "doing", "--json"})
  end)
  if ok_doing and type(t_doing) == "table" then
    for _, t in ipairs(t_doing) do
      if t.title then existing[t.title] = true end
    end
  end

  return existing
end

-- ---------------------------------------------------------------------------
-- Phase: introspect
-- ---------------------------------------------------------------------------

function introspect()
  flow.phase("introspect")
  flow.log("introspect.lua: starting introspect phase")

  -- -------------------------------------------------------------------------
  -- 1. Resolve args
  -- -------------------------------------------------------------------------
  local plan_id  = ctx.args.plan_id
  local days     = ctx.args.days or 30
  local scope    = ctx.args.scope  -- may be nil

  if not plan_id then
    flow.fail("introspect.lua: ctx.args.plan_id is required")
    return
  end

  flow.log("introspect.lua: plan_id=" .. tostring(plan_id)
    .. " days=" .. tostring(days))

  -- -------------------------------------------------------------------------
  -- 2. Read signals
  -- -------------------------------------------------------------------------

  -- 2a. Diagnostic bundle from planar report --json
  flow.log("introspect.lua: reading planar report --json --days " .. tostring(days))
  local report_argv = {"report", "--json", "--days", tostring(days)}
  local bundle = cli.planar_json(report_argv)

  -- 2b. planar-watch read surfaces (always-on observability tables)
  flow.log("introspect.lua: reading planar-watch claims")
  local ok_claims, pw_claims = pcall(function()
    return cli.planar_watch_json({"claims", "--json", "--status", "all"})
  end)

  -- Emit a signals event (informational; the result carries the counts)
  local inv_count = 0
  if type(bundle.invocations) == "table" then
    inv_count = #bundle.invocations
  end
  local claims_stale  = 0
  local handoffs_stale = 0
  if type(bundle.claims) == "table" then
    claims_stale = bundle.claims.stale_claims or 0
  end
  if type(bundle.handoffs) == "table" then
    handoffs_stale = bundle.handoffs.stale_handoffs or 0
  end

  flow.log("introspect.lua: signals — invocations=" .. tostring(inv_count)
    .. " stale_claims=" .. tostring(claims_stale)
    .. " stale_handoffs=" .. tostring(handoffs_stale))

  -- -------------------------------------------------------------------------
  -- 3. Classify signals into the closed taxonomy
  -- -------------------------------------------------------------------------
  -- Each candidate is: { taxonomy, signal_key, body, kind }
  -- taxonomy: "failure-cluster" | "retry-pattern" | "abandoned-workflow" | "gap-feature"
  -- kind:     "question" | "task"

  local candidates = {}

  -- 3a. From invocations: failure-cluster, retry-pattern, gap-feature
  if type(bundle.invocations) == "table" then
    for _, inv in ipairs(bundle.invocations) do
      local vp           = inv.verb_path or "unknown"
      local fail_count   = inv.failure_count or 0
      local success_count = inv.success_count or 0

      if fail_count >= 3 then
        -- retry-pattern: many failures followed by at least one success
        if success_count >= 1 then
          candidates[#candidates + 1] = {
            taxonomy   = "retry-pattern",
            signal_key = vp,
            body       = "verb path '" .. vp .. "' had "
              .. tostring(fail_count) .. " failures and "
              .. tostring(success_count) .. " successes in the past "
              .. tostring(days) .. " days (retry pattern)",
            kind       = "question",
          }
        else
          -- failure-cluster: all (or mostly) failures, no success
          candidates[#candidates + 1] = {
            taxonomy   = "failure-cluster",
            signal_key = vp,
            body       = "verb path '" .. vp .. "' had "
              .. tostring(fail_count) .. " failures and "
              .. tostring(success_count) .. " successes in the past "
              .. tostring(days) .. " days (failure cluster)",
            kind       = "question",
          }
        end
      end
    end
  end

  -- 3b. From failure tail: gap-feature (error category signals missing flags)
  if type(bundle.failure_tail) == "table" then
    local gap_seen = {}
    for _, row in ipairs(bundle.failure_tail) do
      local vp  = row.verb_path or "unknown"
      local cat = row.error_category or ""
      if not gap_seen[vp] then
        if contains_ci(cat, "not_found") or contains_ci(cat, "usage") then
          gap_seen[vp] = true
          candidates[#candidates + 1] = {
            taxonomy   = "gap-feature",
            signal_key = vp,
            body       = "verb path '" .. vp .. "' failed with error category '"
              .. cat .. "' (possible missing flag or unrecognized option); "
              .. "observed in the past " .. tostring(days) .. " days",
            kind       = "question",
          }
        end
      end
    end
  end

  -- 3c. Stale claims → abandoned-workflow
  if claims_stale > 0 then
    candidates[#candidates + 1] = {
      taxonomy   = "abandoned-workflow",
      signal_key = "stale-claims",
      body       = tostring(claims_stale) .. " stale agent claim(s) detected "
        .. "(active beyond the 24-hour TTL threshold); possible abandoned workflows",
      kind       = "task",
    }
  end

  -- 3d. Stale handoffs → abandoned-workflow
  if handoffs_stale > 0 then
    candidates[#candidates + 1] = {
      taxonomy   = "abandoned-workflow",
      signal_key = "stale-handoffs",
      body       = tostring(handoffs_stale) .. " stale handoff(s) detected "
        .. "(pending beyond 24 hours without being resumed); possible abandoned workflows",
      kind       = "task",
    }
  end

  -- 3e. From planar-watch claims (extra stale count cross-check)
  if ok_claims and type(pw_claims) == "table" and type(pw_claims.claims) == "table" then
    local stale_in_watch = 0
    for _, cl in ipairs(pw_claims.claims) do
      local st = cl.status or ""
      if st == "stale" then
        stale_in_watch = stale_in_watch + 1
      end
    end
    -- Only file if the watch surface reveals stale claims the bundle didn't (guard doubles)
    if stale_in_watch > 0 and claims_stale == 0 then
      candidates[#candidates + 1] = {
        taxonomy   = "abandoned-workflow",
        signal_key = "stale-claims",
        body       = tostring(stale_in_watch)
          .. " stale claim(s) observed via planar-watch claims "
          .. "(may have appeared after the report window closed)",
        kind       = "task",
      }
    end
  end

  flow.log("introspect.lua: classified " .. tostring(#candidates) .. " candidate(s)")

  -- -------------------------------------------------------------------------
  -- 4. Dedup + file
  -- -------------------------------------------------------------------------
  -- Collect existing titles immediately before the filing loop so the set
  -- reflects any findings added earlier in this same pass.
  local existing = collect_existing_titles(plan_id)

  local filed   = 0
  local skipped = 0
  local filed_titles = {}
  local skipped_titles = {}

  -- Dedup candidates by title before iterating (avoid double-filing in one pass)
  local seen_this_pass = {}

  for _, cand in ipairs(candidates) do
    local title = cand.taxonomy .. ": " .. cand.signal_key

    -- Skip if already filed in a prior run
    if existing[title] then
      skipped = skipped + 1
      skipped_titles[#skipped_titles + 1] = title
      flow.log("introspect.lua: already present: " .. title)
    -- Skip if filed earlier in this same pass
    elseif seen_this_pass[title] then
      skipped = skipped + 1
      flow.log("introspect.lua: dedup within pass: " .. title)
    else
      seen_this_pass[title] = true

      -- Build the add argv
      local add_argv
      if cand.kind == "question" then
        add_argv = {"question", "add", title,
          "--plan", tostring(plan_id),
          "--body", cand.body}
        if scope then
          add_argv[#add_argv + 1] = "--scope"
          add_argv[#add_argv + 1] = scope
        end
      else
        add_argv = {"task", "add", title,
          "--plan", tostring(plan_id),
          "--body", cand.body}
        if scope then
          add_argv[#add_argv + 1] = "--scope"
          add_argv[#add_argv + 1] = scope
        end
      end

      local ok_add, add_err = pcall(function()
        cli.planar(add_argv)
      end)
      if ok_add then
        filed = filed + 1
        filed_titles[#filed_titles + 1] = title
        flow.log("introspect.lua: filed " .. cand.kind .. ": " .. title)
      else
        flow.log("introspect.lua: WARNING: failed to file '" .. title
          .. "': " .. tostring(add_err))
      end
    end
  end

  -- -------------------------------------------------------------------------
  -- 5. Result
  -- -------------------------------------------------------------------------
  local summary
  if filed == 0 and skipped == 0 then
    summary = "nothing noteworthy in the past " .. tostring(days) .. " days"
  else
    summary = "filed=" .. tostring(filed) .. " skipped=" .. tostring(skipped)
  end

  flow.log("introspect.lua: " .. summary)

  flow.result({
    filed         = filed,
    skipped       = skipped,
    candidates    = #candidates,
    summary       = summary,
    filed_titles  = filed_titles,
    skipped_titles = skipped_titles,
    window_days   = days,
  })
end
