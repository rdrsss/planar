/// @file engine.cppm
/// @brief `planar.cmd.planar_execute.engine` — the `run` verb's execution
/// path, as far as this milestone carries it (plan 996, task 6107).
///
/// Port target: `runWorkflow` in zig/src/cmd/planar-execute/main.zig.
///
/// ## What is ported and what is deferred, and exactly where the seam is
///
/// The seam is the workflow FILE READ, and it is not an arbitrary place to
/// stop — it is the last step before Lua enters the picture. `runWorkflow`
/// reads the file first, and its failure is an operator-visible,
/// oracle-captured path with its own message and its own exit code:
///
///     $ planar-execute run x.lua --phase p          # x.lua absent
///     stderr: "planar-execute: cannot read workflow: x.lua\n"
///     exit:   1
///
/// Note the exit code. Every OTHER planar-execute failure shape is exit 2
/// (bad usage); this one is 1, because `LoadFailed` is not `BadUsage`. A
/// port that collapsed the two — the natural mistake, since both are
/// "something was wrong with the arguments" from a distance — would be
/// wrong here and nowhere else. `parity.t.cpp` pins 0, 1 and 2 all
/// produced by this binary so a collapsed mapping cannot pass.
///
/// Everything from `luaL_newstate()` onward is DEFERRED WITH ITS
/// DEPENDENCY: there is no Lua in this tree. `cmake/dependencies.cmake`
/// names "Lua 5.5 — planar-execute sandbox" in its inventory comment but
/// declares no `CPMAddPackage` for it, and there is no `engine_execute`
/// bucket under `src/lib/engine/` — the sandboxed stdlib
/// (`openSandboxedLibs`), the frozen host-fn manifest
/// (`installHostSurface`, `ALLOWED_HOST_FNS`/`DENIED_HOST_FNS`), the
/// phase-resolution and the `flow.result` marshalling are all unported.
/// That is a layer-2 bucket port plus a vendored dependency, which is a
/// milestone, not a corner of this one.
///
/// So a `run` whose file DOES read exits 64 (`not implemented`) with a
/// message naming the gap. That is a divergence from the oracle and it is
/// declared here rather than hidden: it is the honest shape, because the
/// alternative — printing `{}` and exiting 0, which is what the oracle does
/// for a workflow that declares no result — would be a SILENT wrong
/// answer, and a caller cannot tell a deferred engine from an empty result.
///
/// ## What must NOT be added here later
///
/// No spawn seam. Not a `std::process`/`fork` helper, not an "exec" host
/// function, not a re-entrant path back into an LLM client. plan 633's D5
/// is the reason this binary exists in its current form at all, and
/// `cli.t.cpp` holds the advertised-surface half of that lock.
module;

export module planar.cmd.planar_execute.engine;

import std;

namespace planar::cmd::execute {

/// @brief The outcome of attempting a `run`, mapped to a process exit code
/// by the caller.
export enum class run_outcome : std::uint8_t {
  /// @brief The workflow file could not be read. Oracle: exit 1, stderr
  /// `planar-execute: cannot read workflow: <path>`.
  load_failed,
  /// @brief The file read, but the Lua engine is unported. Exit 64.
  engine_unported,
};

/// @brief Attempt to read the workflow named by `path`.
///
/// Split out from the run path proper so the ported half is testable
/// without a Lua state: this is the whole of what this milestone executes.
/// Not confined — the workflow path is operator input, not script-
/// controlled (the Zig original says so; `fs.*` confinement applies to
/// in-script IO, which is the deferred half).
/// @param path The workflow file path.
/// @return The file's bytes, or unset when it cannot be read.
export auto read_workflow(const std::filesystem::path& path) -> std::optional<std::string>;

/// @brief Run the ported half of `planar-execute run`: read the workflow,
/// then stop at the Lua seam.
/// @param path The workflow file path.
/// @param err The stream diagnostics are written to (stderr for this
/// binary; every planar-execute diagnostic goes there, never stdout).
/// @return `load_failed` when the file could not be read, otherwise
/// `engine_unported`.
export auto run_workflow(const std::filesystem::path& path, std::ostream& err) -> run_outcome;

} // namespace planar::cmd::execute
