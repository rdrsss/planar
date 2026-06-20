--[[ @meta
name: example
description: Reference workflow demonstrating the planar-execute host surface.
phases: setup
seam: planar schema (no DB dependency)
--]]

-- example.lua — reference workflow for planar-execute.
--
-- planar-execute is a deterministic, spawn-free Lua workflow engine. It
-- provides a frozen set of host functions (cli / git / fs / flow / ctx) and
-- NO general exec or LLM-spawning primitive. The caller drives the
-- orchestration loop; the engine handles one discrete phase per invocation.
--
-- How to run this file:
--
--   planar-execute run workflows/example.lua --phase setup
--
-- Each phase is a top-level Lua function. The engine:
--   1. Loads this file in the sandbox (os/io/require are nil).
--   2. Calls the named phase function.
--   3. Marshals the flow.result(table) payload to JSON on stdout and exits.
--
-- The engine exits 0 on success and non-zero if the phase errors or calls
-- flow.fail(msg).

-- ---------------------------------------------------------------------------
-- Phase: setup
--
-- Demonstrates the core host surface with inline comments explaining each
-- call. This phase is genuinely runnable with no --args and no --worktree;
-- it only reads the planar schema (no DB dependency).
-- ---------------------------------------------------------------------------
function setup()
  -- flow.phase(name) records which phase is executing. Informational; the
  -- engine already knows the phase name from --phase, but logging it here
  -- makes the phase boundary visible in engine diagnostics.
  flow.phase("setup")

  -- flow.log(msg) writes a human-readable message to stderr. The stdout
  -- channel is reserved for the clean flow.result JSON payload.
  flow.log("example.lua: setup phase starting")

  -- cli.planar_json(argv) shells `planar <argv...>` and returns the parsed
  -- JSON as a Lua table. The binary is hardcoded (no general exec). Passing
  -- {"schema"} here reads the planar command schema — a read-only verb with
  -- no DB dependency, so this phase is fully self-contained.
  local schema = cli.planar_json({"schema"})

  flow.log("example.lua: planar root = " .. tostring(schema.root))
  flow.log("example.lua: schema version = " .. tostring(schema.schemaVersion))

  -- flow.result(table) captures the final payload. The engine marshals this
  -- to JSON on stdout after the phase returns. Only the LAST call to
  -- flow.result wins; subsequent calls overwrite earlier ones.
  flow.result({
    ok            = true,
    phase         = "setup",
    planar_root   = schema.root,
    schema_version = schema.schemaVersion,
  })
end
