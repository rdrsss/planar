/// @file execute.cppm
/// @brief `planar.engine_execute` — the sandboxed, spawn-free Lua workflow
/// host behind `planar-execute run` (plan 996, task 6042; plan 633 for the
/// binary itself).
///
/// Port target: `zig/src/cmd/planar-execute/host.zig` plus `runWorkflow` in
/// `zig/src/cmd/planar-execute/main.zig`.
///
/// ## THE EXPORTED SURFACE OF THIS MODULE IS ITSELF A BOUNDARY
///
/// Read the export list below and notice what is NOT on it: no `lua_State`,
/// no way to obtain one, no way to register a function into one, and above
/// all no process-spawning primitive. This module DOES fork and exec — it
/// has to, because `cli.planar(...)` and `git.head_sha()` are shells — but
/// that runner is defined inside `host.cpp` with internal linkage and is
/// unreachable from outside this module by construction. There is no header
/// to include and no exported declaration to name. A future caller that
/// wants "just a small exec helper, it's already written" cannot have one
/// without deliberately exporting it, and that edit is visible in this file.
///
/// That matters more here than anywhere else in the tree. An earlier
/// planar-execute grew re-entrant headless LLM spawning and became a harness
/// in its own right, which is why it was extracted to a separate project;
/// this revival exists to hold that line (plan 633 D5). The three
/// independent locks are:
///
///   1. CONFIGURE TIME — `cmake/architecture.cmake` treats this target as an
///      execute carrier and FATALs if it (or anything reaching it) reaches
///      `planar_db`. Adding `db` to DEPENDS fails the build with a named
///      diagnostic; a test could only observe a handle this module chose to
///      open, so the guard sits earlier than any test can.
///   2. COMPILE TIME — `allowed_host_fns()` and `denied_host_fns()` are
///      `constexpr` tables and `manifest.cpp` carries a `static_assert` that
///      their name sets are disjoint. A registrar entry named `spawn` or
///      `exec` cannot compile.
///   3. RUN TIME — `registered_host_surface()` builds a real state, installs
///      the surface, and enumerates what is ACTUALLY reachable from a
///      workflow. `surface.t.cpp` compares that live enumeration against the
///      frozen manifest. See "the frozen manifest" below for why that is a
///      different and stronger claim than comparing two constants.
///
/// ## The frozen manifest, and what its test actually proves
///
/// `allowed_host_fns()` is a table of `(table, name)` pairs — the ENTIRE
/// host-call capability surface of a workflow. Twenty-five entries, and the
/// number is pinned.
///
/// The test that matters is not "does `fs.read` read a file". It is
/// `registered_host_surface() == allowed_host_fns()`, where the left side is
/// produced by walking the five global tables of a live `lua_State` with
/// `lua_next` after `install_host_surface` has run. That comparison fails on
/// every way the surface can grow or shift:
///
///   - a new host function registered but not declared (the growth case the
///     whole milestone guards against — the registrar is data-driven off the
///     manifest, so this specifically catches a hand-written `lua_setfield`
///     added beside the loop);
///   - a declared function silently not registered (a dispatch arm dropped);
///   - a function moved between tables (`fs.read` becoming `cli.read`);
///   - a sixth global table appearing;
///   - a non-function value appearing on one of the five tables (caught
///     separately, since `ctx` legitimately carries three non-function
///     fields and the enumeration is typed).
///
/// What it does NOT prove: that any individual host function is correct.
/// That is deliberate and is the milestone's stated priority — the risk this
/// binary carries is the surface quietly widening, not `git.clean` gaining
/// an off-by-one.
///
/// ## Sandbox
///
/// `open_sandboxed_libs` opens base, table, string, math and utf8, then
/// removes the escape hatches. Every removal below was derived by RUNNING
/// the oracle and enumerating its live globals (`_G` and each table), never
/// read out of the Zig source:
///
///   nil globals:   dofile  load  loadfile  require  (plus `loadstring`,
///                  which 5.5 does not define anyway)
///   never opened:  os  io  debug  coroutine  package
///   nil'd fields:  math.random  math.randomseed
///
/// `coroutine` is absent rather than trimmed: the hand-back model is one
/// clean process per phase, and a coroutine park awaiting an external worker
/// is exactly where re-entrant spawning regrew last time.
///
/// With `os` and `io` gone the only clock and the only seed a workflow can
/// reach are `ctx.now` and `ctx.seed`, both injected by the caller and both
/// defaulting to 0 — so a run is reproducible by default.
///
/// `sandbox_globals()` and `sandbox_table_entries()` exist so that list can
/// be asserted against a live state instead of against this comment.
module;

export module planar.engine_execute;

import std;

namespace planar::engine::execute {

// ---------------------------------------------------------------------------
// The frozen manifest
// ---------------------------------------------------------------------------

/// @brief One registered host function: the table that owns it and its field
/// name. Host functions are never globals — a workflow reaches the host only
/// through `cli.*` / `git.*` / `fs.*` / `flow.*` / `ctx.*`.
export struct host_fn {
  std::string_view table; ///< Owning table: cli / git / fs / flow / ctx.
  std::string_view name;  ///< Field name on that table.

  /// @brief Value equality, so a live enumeration can be compared to the
  /// manifest directly.
  /// @param other The other entry.
  /// @return `true` when both halves match.
  friend auto operator==(host_fn const& lhs, host_fn const& other) -> bool = default;
};

/// @brief The five host tables, in registration order.
/// @return The table names.
export auto host_tables() -> std::span<const std::string_view>;

/// @brief The complete host-call capability surface (D7's allowlist).
///
/// The single source of truth: the registrar iterates THIS to build the
/// tables, so an implementation cannot drift from the declaration in the
/// "declared but absent" direction, and `surface.t.cpp`'s live enumeration
/// closes the "present but undeclared" direction.
/// @return The manifest, sorted by (table, name) within each table group.
export auto allowed_host_fns() -> std::span<const host_fn>;

/// @brief Names that must never appear anywhere on the host surface — the
/// spawn / general-exec primitives that grew this binary into a harness.
///
/// Asserted disjoint from `allowed_host_fns()` by a `static_assert`, and
/// asserted absent from a LIVE state by `surface.t.cpp` (which checks the
/// globals too, not just the five tables — a `spawn` global would be just as
/// reachable as a `cli.spawn`).
/// @return The denylist.
export auto denied_host_fns() -> std::span<const std::string_view>;

/// @brief The only binaries a `cli.*` host function may shell.
/// @return `planar`, `planar-agent`, `planar-watch`.
export auto allowed_cli_bins() -> std::span<const std::string_view>;

// ---------------------------------------------------------------------------
// Pure guards (no Lua, no process, no filesystem)
// ---------------------------------------------------------------------------

/// @brief Is `<bin> <argv...>` inside the deterministic workflow capability
/// set?
///
/// A SECOND allowlist behind the binary allowlist: being allowed to shell
/// `planar` is not being allowed to run every `planar` verb. `planar`
/// commands are matched as a `(verb, subcommand)` pair (plus four
/// single-token verbs); `planar-agent` and `planar-watch` match on the first
/// token alone.
/// @param bin The binary name (not a path).
/// @param argv The arguments after the binary name.
/// @return `true` when the command may run.
export auto command_allowed(std::string_view bin, std::span<const std::string> argv) -> bool;

/// @brief Does `value` look like a git object id (7–64 hex characters)?
/// @param value The candidate.
/// @return `true` when it does.
export auto is_hex_object_id(std::string_view value) -> bool;

/// @brief Is `value` safe to pass to git as a ref?
///
/// Rejects the empty string, anything starting `-` (which git would read as
/// an option), anything containing `..` (the range syntax, and the parent
/// component), and any character outside `[A-Za-z0-9/_.~^-]`.
/// @param value The candidate ref.
/// @return `true` when it is safe.
export auto safe_git_ref(std::string_view value) -> bool;

/// @brief Is `rel` an acceptable sandbox-relative path?
///
/// Rejects empty, absolute, backslash-bearing, and any path with an empty,
/// `.` or `..` component. This is the CHEAP half of `fs.*` confinement; the
/// expensive half is the component-by-component no-follow directory walk in
/// `host.cpp`, which is what actually defeats a symlink swapped in after
/// this check passes.
/// @param rel The candidate relative path.
/// @return `true` when the shape is acceptable.
export auto validate_confined_rel(std::string_view rel) -> bool;

/// @brief Format a double the way the oracle's `{d}` does: shortest
/// round-tripping decimal, never scientific notation.
///
/// Split out and exported because it is the one piece of `flow.result`
/// marshalling with a non-obvious contract and it is worth pinning directly.
/// The oracle prints `1e300` as three hundred and one digits and `3e-7` as
/// `0.0000003`; `std::format("{}", d)` would print `1e+300` and `3e-07`.
/// @param value The number.
/// @return The formatted text.
export auto format_double(double value) -> std::string;

// ---------------------------------------------------------------------------
// Running a workflow
// ---------------------------------------------------------------------------

/// @brief Everything one `planar-execute run` invocation needs.
export struct run_config {
  std::string  workflow_name; ///< The workflow path, for diagnostics only.
  std::string  source;        ///< The workflow's bytes, already read.
  std::string  phase;         ///< The phase function to call.
  std::string  args_json;     ///< `--args` payload; empty means `{}`.
  std::string  worktree;      ///< `--worktree`; empty disables `git.*`.
  std::string  sandbox_root;  ///< `--sandbox-root`; empty disables `fs.*`.
  std::string  bin_dir;       ///< Directory holding the trusted sibling binaries.
  std::int64_t now  = 0;      ///< `ctx.now`.
  std::int64_t seed = 0;      ///< `ctx.seed`.
};

/// @brief How a run ended. Every non-`ok` value is exit 1 at the binary —
/// the oracle maps everything except `BadUsage` (which `run_workflow` cannot
/// produce) to 1, so this enum deliberately does not pretend to carry more
/// exit-code resolution than the oracle has.
export enum class run_status : std::uint8_t {
  ok,            ///< The phase ran; the payload (or `{}`) is on stdout.
  init_failed,   ///< The Lua state could not be created.
  load_failed,   ///< The chunk failed to compile, or its top level errored.
  phase_missing, ///< No global of that name, or it is not a function.
  phase_failed,  ///< The phase raised, or called `flow.fail`.
};

/// @brief Load a workflow into a fresh sandbox, install the host surface,
/// call one phase, and marshal its `flow.result` payload.
///
/// Writes the JSON payload (or `{}`) plus a newline to `out` on success, and
/// every diagnostic — including `flow.log` lines DURING the run — to `err`.
/// stdout stays a clean JSON channel; that separation is the oracle's and is
/// pinned by the parity tests.
/// @param config The run configuration.
/// @param out The stdout stream.
/// @param err The stderr stream.
/// @return How the run ended.
export auto run_workflow(run_config const& config, std::ostream& out, std::ostream& err) -> run_status;

// ---------------------------------------------------------------------------
// Live introspection — the frozen-manifest and sandbox locks
// ---------------------------------------------------------------------------

/// @brief One name/type pair observed on a live sandboxed state.
export struct value_entry {
  std::string name; ///< The key.
  std::string type; ///< Lua's own type name: function, table, number, string, …
};

/// @brief Build a sandboxed state with the host surface installed, then
/// enumerate every FUNCTION reachable on the five host tables.
///
/// This is the frozen-manifest lock's left-hand side. It observes the state a
/// workflow would actually see rather than re-reading the manifest, which is
/// the entire point: a registration added outside the manifest-driven loop
/// shows up here and nowhere else.
/// @return The live `(table, name)` pairs, sorted.
export auto registered_host_surface() -> std::vector<std::pair<std::string, std::string>>;

/// @brief Enumerate `_G` on a live sandboxed state with the host surface
/// installed.
/// @return Every global's name and Lua type, sorted by name.
export auto sandbox_globals() -> std::vector<value_entry>;

/// @brief Enumerate one table on a live sandboxed state.
/// @param table The global table's name (e.g. `math`, `ctx`).
/// @return Its entries, sorted by name; empty when the global is absent or
/// is not a table.
export auto sandbox_table_entries(std::string_view table) -> std::vector<value_entry>;

/// @brief The `_VERSION` string of the linked Lua.
///
/// Pinned by a test because the sandbox's observable surface is
/// version-sensitive (5.5 adds `table.create`; its `math` carries
/// `acos`/`frexp`/`ldexp`, a 5.4 build's does not), so a silent Lua bump
/// would otherwise change what the sandbox enumeration sees while every host
/// function still appeared to work.
/// @return e.g. `"Lua 5.5"`.
export auto lua_version() -> std::string;

} // namespace planar::engine::execute
