/// @file cli.cppm
/// @brief `planar.cmd.planar_execute.cli` — `planar-execute`'s hand-rolled
/// argument surface: the usage text, the `run` verb's parser, and the
/// verb-dispatch decision (plan 996, task 6107).
///
/// Port target: `main`, `usage` and `parseRunArgs` in
/// zig/src/cmd/planar-execute/main.zig.
///
/// ## Why this binary parses its own arguments
///
/// It does NOT use `planar.cli`. That is inherited, not invented:
/// zig/integration_tests/capability_boundary_test.zig says so directly —
/// "Unlike the other four binaries, planar-execute uses manual arg parsing
/// (not the etcli-zig COMMANDS table), so we cannot apply the
/// parseHelpVerbs/assertExactSet pattern" — and it is the same reason
/// CLAUDE.md records for excluding this binary from
/// `tools/cli_usage_lint.zig`: it has NO `schema` catalog to dump. Wiring
/// it onto the shared parser now would manufacture a catalog the reference
/// binary does not have and would make this binary look like a fifth
/// member of a family it is deliberately not in.
///
/// That exemption ended with decision 1030 (D18, plan 1033 M0, task 6486):
/// the oracle is gone, and `planar-execute schema` now emits the same flat
/// JSON catalog the other four binaries do so `cli_usage_lint` can police
/// authored references to its verbs. The PARSER did not move — the catalog
/// is a description built in `planar.cmd.planar_execute.catalog` and pinned
/// against `parse_run_args` by test; every argv shape above is unchanged.
///
/// ## The capability boundary here is what is ABSENT
///
/// `planar-execute` holds NO SQLite handle at all — it reaches Planar
/// state only by shelling `planar`/`planar-agent` — and exposes NO
/// model-spawning host function. An earlier `planar-execute` grew
/// re-entrant headless LLM spawning and became a harness in its own right,
/// which is why it was extracted to a separate project; the revival reins
/// that back in (CLAUDE.md § four-binary boundary, plan 633 D5/D7).
///
/// Both absences are enforced mechanically rather than by review:
///
///   - NO DATABASE. `src/cmd/planar-execute/CMakeLists.txt` links neither
///     `planar_db` nor anything that reaches it, and
///     `cmake/architecture.cmake` FATALs at CONFIGURE TIME if that ever
///     changes — the target is named so the literal `cmd_planar_execute`
///     arm of the execute-carrier check fires, and the edge-derived arm
///     would fire too the moment an `engine_execute` bucket lands. The
///     proof is not a comment: `cli.t.cpp` asserts this translation unit
///     imports no `planar.db` module and the build itself refuses the edge.
///   - NO SPAWN AFFORDANCE. `usage_text()` below is checked against the
///     D7 denied-host-fn word list in `cli.t.cpp`, reproducing
///     capability_boundary_test.zig's "advertises no spawn affordance"
///     assertion including its five documented exclusions.
///
/// ## Oracle behaviour, captured not inferred
///
/// Every byte and every code below was captured by running
/// `zig/zig-out/bin/planar-execute` in a pinned scratch arena. Two of them
/// are the sort a reasonable port gets wrong:
///
///   - THE USAGE TEXT GOES TO STDERR, ALWAYS — including on `--help`,
///     where stdout stays completely empty and the exit code is 0. Every
///     other binary in this tree writes help to stdout.
///   - A BARE INVOCATION IS EXIT 2, not 0 and not a help request:
///     `planar-execute` with no arguments prints the usage to stderr and
///     exits 2, while `planar-execute --help` prints the identical bytes
///     and exits 0. Same output, different code.
///
///   argv                              stdout  stderr                     exit
///   (none)                            —       usage                      2
///   --help / -h / help                —       usage                      0
///   any other first token             —       "unknown verb: X" + usage   2
///   run                               —       usage                      2
///   run wf.lua                        —       usage                      2   (no --phase)
///   run wf.lua --phase p              —       "cannot read workflow: …"   1   (file absent)
///   schema                            catalog —                          0   (task 6486, D18)
module;

export module planar.cmd.planar_execute.cli;

import std;

namespace planar::cmd::execute {

/// @brief The usage/banner text, byte-for-byte as the oracle emits it —
/// trailing newline included.
///
/// A COMPLETE payload, in the terminator vocabulary the M4 boundary review
/// settled: the caller writes it verbatim and appends nothing. The Zig
/// original's multiline string literal ends with a blank continuation line,
/// which is a final `\n`; dropping it would silently shorten every one of
/// the six argv shapes above by one byte.
/// @return The usage text.
export auto usage_text() -> std::string_view;

/// @brief What `parse_run_args` produces on success — the `run` verb's one
/// positional and four flags.
export struct run_args {
  std::string workflow;     ///< The workflow file path (first positional; required).
  std::string phase;        ///< The `--phase` value (required).
  std::string args_json;    ///< The `--args` JSON blob, or empty.
  std::string worktree;     ///< The `--worktree` directory, or empty.
  std::string sandbox_root; ///< The `--sandbox-root` directory, or empty.
};

/// @brief Parse the `run` verb's arguments (everything after `run`).
///
/// Reproduces `parseRunArgs` exactly, including the details that are easy
/// to "improve" into a divergence: an unrecognised `--`-prefixed token is a
/// usage failure rather than being ignored; a SECOND bare positional is a
/// usage failure rather than overwriting the first; a flag as the last
/// token (its value missing) is a usage failure; and both `workflow` and
/// `phase` are required, checked only after the whole loop has run.
/// @param args The tokens after the `run` verb.
/// @return The parsed arguments, or unset on any usage failure (the
/// oracle's single `BadUsage`, which carries no distinguishing message —
/// every failure prints the same usage text and exits 2).
export auto parse_run_args(std::span<const std::string> args) -> std::optional<run_args>;

/// @brief Which top-level shape an argv resolves to.
export enum class verb : std::uint8_t {
  none,    ///< No arguments at all: usage, exit 2.
  help,    ///< `--help` / `-h` / `help`: usage, exit 0.
  run,     ///< The `run` verb.
  schema,  ///< The `schema` verb: the JSON catalog on stdout, exit 0 (task 6486).
  unknown, ///< Anything else: "unknown verb" + usage, exit 2.
};

/// @brief Classify `argv` (including argv[0]) into a top-level shape.
///
/// The Zig original tests for `run` before it tests for `--help`, and this
/// function keeps that order for readability — but a break-probe showed the
/// order is NOT observable and the claim that it is would have been wrong:
/// the two arms match disjoint tokens, so no argv reaches both. (Swapping
/// them left every test green.) What IS observable, and is pinned in
/// `parity.t.cpp`, is the CONSEQUENCE of `run` being a verb rather than a
/// help-bearing command: `planar-execute run --help` is a `run` invocation
/// whose arguments fail to parse — usage on stderr, exit 2 — not a help
/// request, because `--help` reaches `parse_run_args` as an unrecognised
/// long flag. THAT is what a port could get wrong, by teaching
/// `parse_run_args` to recognise `--help`.
/// @param argv The full argument vector, including argv[0].
/// @return The classified shape.
export auto classify(std::span<const std::string> argv) -> verb;

} // namespace planar::cmd::execute
