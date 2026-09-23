/// @file engine.cppm
/// @brief `planar.cmd.planar_execute.engine` — the `run` verb's execution
/// path (plan 996, tasks 6107 and 6042).
///
/// Port target: `runWorkflow` in zig/src/cmd/planar-execute/main.zig.
///
/// ## What this layer does, and what it deliberately does not
///
/// It reads the workflow file, resolves the directory holding the trusted
/// sibling binaries, and hands both to `planar.engine_execute`. It owns no
/// Lua state, registers no host function and spawns no process; all of that
/// lives one layer down, where the architecture guard's execute-carrier rule
/// applies to it.
///
/// The FILE READ stays here rather than moving down with the rest, because
/// it is the one step that is not script-controlled. The workflow path is
/// operator input — `fs.*` confinement governs what a running script may
/// touch, not which script the operator chose to run — so confining it would
/// be wrong, and leaving an unconfined read inside the confined module would
/// be a confusing place to put it.
///
/// Its failure is an operator-visible, oracle-captured path with its own
/// message and its own exit code:
///
///     $ planar-execute run x.lua --phase p          # x.lua absent
///     stderr: "planar-execute: cannot read workflow: x.lua\n"
///     exit:   1
///
/// Note the exit code. Every OTHER planar-execute failure shape is exit 2
/// (bad usage); this one is 1, because `LoadFailed` is not `BadUsage`. A
/// port that collapsed the two — the natural mistake, since both are
/// "something was wrong with the arguments" from a distance — would be wrong
/// here and nowhere else. `parity.t.cpp` pins 0, 1 and 2 all produced by
/// this binary so a collapsed mapping cannot pass.
///
/// ## The Lua engine is no longer deferred
///
/// Task 6107 stopped at `luaL_newstate()` and exited 64 naming the gap,
/// because there was no Lua in the tree and no `engine_execute` bucket. Both
/// exist now (task 6042): `cmake/dependencies.cmake` pins Lua 5.5.0 and
/// `src/lib/engine/execute/` holds the sandbox, the frozen twenty-five
/// function host surface and the run loop. Exit code 64 is gone from this
/// binary — a `run` over a readable workflow now runs it.
///
/// All twenty-five host functions are complete as of task 6125, including
/// `ctx.brief`, whose body compiles a real coder brief from the ported
/// `state`/`schema`/`brief` slice of the retired Zig harness's
/// `state.zig`/`schema.zig`/`brief.zig`. See
/// src/lib/engine/execute/CMakeLists.txt for exactly which slice — the
/// harness-only helpers those files also carried are deliberately not
/// ported (no consumer on this binary's host surface).
///
/// ## What must NOT be added here later
///
/// No spawn seam. Not a `std::process`/`fork` helper, not an "exec" host
/// function, not a re-entrant path back into an LLM client. plan 633's D5 is
/// the reason this binary exists in its current form at all, `cli.t.cpp`
/// holds the advertised-surface half of that lock, and
/// `src/lib/engine/execute/surface.t.cpp` holds the live-state half.
module;

export module planar.cmd.planar_execute.engine;

import std;
import planar.cmd.planar_execute.cli;

namespace planar::cmd::execute {

/// @brief The outcome of attempting a `run`, mapped to a process exit code
/// by the caller.
export enum class run_outcome : std::uint8_t {
  /// @brief The phase ran to completion; its payload is on stdout.
  ok,
  /// @brief The workflow file could not be read. Oracle: exit 1, stderr
  /// `planar-execute: cannot read workflow: <path>`.
  load_failed,
  /// @brief The sandbox, the chunk, the phase lookup or the phase itself
  /// failed. Every one of these is exit 1 at the oracle — it maps every
  /// engine error except `BadUsage` (which this path cannot produce) to 1 —
  /// so they are deliberately not split further here.
  engine_failed,
};

/// @brief Attempt to read the workflow named by `path`.
///
/// Not confined — the workflow path is operator input, not script-
/// controlled. See this module's header.
/// @param path The workflow file path.
/// @return The file's bytes, or unset when it cannot be read.
export auto read_workflow(const std::filesystem::path& path) -> std::optional<std::string>;

/// @brief The directory containing the running executable.
///
/// `cli.planar(...)` resolves `planar` as a SIBLING of this binary rather
/// than through `PATH`, so what a workflow shells is the planar that shipped
/// alongside this planar-execute — not whatever a `PATH` happens to name
/// first. That is the oracle's behaviour (`executableDirPathAlloc`), and it
/// is what makes the CLI allowlist meaningful: allowlisting the NAME
/// `planar` would be worth little if the name resolved anywhere.
/// @return The directory, or empty when it cannot be determined (which
/// disables every `cli.*` host function with a named error rather than
/// falling back to a search path).
export auto executable_dir() -> std::string;

/// @brief Run `planar-execute run`: read the workflow, then execute the
/// named phase in the sandbox.
/// @param args The parsed `run` arguments.
/// @param out The stdout stream — the clean JSON result channel.
/// @param err The stderr stream; every diagnostic goes here, including on
/// the success path.
/// @return How the run ended.
export auto run_workflow(run_args const& args, std::ostream& out, std::ostream& err) -> run_outcome;

} // namespace planar::cmd::execute
