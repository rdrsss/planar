/// @file worktree_gate.cppm
/// @brief `planar.cmd.planar.worktree_gate` — refuse planning verbs
/// invoked from inside a git worktree (plan 996, task 6137).
///
/// Port of zig/src/cmd/planar/worktree_gate.zig. It was deferred at task
/// 6105 WITH its git-subprocess dependency and named as an omission in
/// `planar.cmd.planar.scope`'s header. Task 6135 turned that omission from
/// theoretical into observable — wiring `task add` gave the integration
/// suite a working planning verb to run from a worktree, and the refusal
/// came back MISSING: exit 0, with a real row written, in the exact
/// situation the gate exists to refuse. `planar.git` (tasks 6128/6137) is
/// the seam that was missing; this module is the gate on top of it.
///
/// ## Where it fires, and why BEFORE the parser
///
/// `check` runs inside `dispatch::run` before `root.parse`, against a
/// standalone argv walk rather than CLI11's resolved path. That ordering
/// is the oracle's and it is deliberate: a planning verb run from a
/// worktree must exit 8 even when its arguments ALSO fail to parse. Gating
/// after the parse would report the parse error (exit 1) and leave the
/// operator to fix their flags, cd, and only then discover the refusal.
/// The walk is intentionally simple — it needs the path, not the values.
///
/// ## Exit code 8, outside the mapped range
///
/// Codes 0-7 are spoken for (`exit_code_for`: success, generic, input,
/// conflict, scope, already-exists, schema). 8 gives a script a distinct
/// value for "you ran a planning verb from a worktree" versus "your scope
/// was wrong" (5). Like the oracle, the gate returns it directly rather
/// than routing it through the error-kind mapping, so that mapping stays
/// load-bearing in exactly one place.
///
/// ## `--scope` does NOT override this
///
/// The rule is about WHERE the verb runs, not which scope it targets, so
/// the detection consults cwd-derived state only and no flag suppresses
/// it. The scenario suite pins this with a `plan create ... --scope <slug>`
/// case that must still exit 8.
///
/// ## Bypass: two layers, and the outer one is compile-time
///
/// `PLANAR_DISABLE_WORKTREE_GATE=1` is honoured ONLY in a build configured
/// with `-DPLANAR_TEST_BINARY=ON` (the `debug` preset, which is also what
/// the `test-parity-cpp` lane consumes). In a production build the branch
/// is `if constexpr`-dead and the environment cannot defeat the gate at
/// all. This mirrors the Zig two-layer design and exists for the same
/// reason: the integration harness injects the variable so fixture paths
/// that incidentally live under `.worktrees/` do not trip the gate for
/// tests that are not about it, and the worktree scenarios un-set it to
/// exercise the real refusal. A value that is empty or starts with `0` is
/// "not set", exactly as the oracle reads it.
module;

export module planar.cmd.planar.worktree_gate;

import std;
import cli11;
import planar.cmd.planar.context;

namespace planar::cmd::worktree_gate {

/// @brief The refusal's exit code. See this file's header for why it sits
/// outside `exit_code_for`'s 0-7 range.
export constexpr int exit_code_worktree_refusal = 8;

/// @brief Is the environment-variable bypass compiled into this build?
///
/// False in every build that does not set `-DPLANAR_TEST_BINARY=ON`.
/// Exposed so a test can assert the posture of the binary it is running
/// in rather than assuming it.
///
/// @return True when this build was configured with
/// `-DPLANAR_TEST_BINARY=ON`, and therefore honours
/// `PLANAR_DISABLE_WORKTREE_GATE`; false in every production build, where
/// the environment check is `if constexpr`-dead.
export auto bypass_compiled_in() -> bool;

/// @brief Inspect argv and refuse when it names a planning verb AND the
/// cwd lives inside a git worktree.
///
/// @param ctx The invocation context, for the cwd, the environment and
/// the error stream.
/// @param root The command tree, walked to resolve argv to a verb path.
/// @return The refusal exit code when the verb is refused, or unset when
/// the invocation is allowed to proceed.
export auto check(context& ctx, const CLI::App& root) -> std::optional<int>;

/// @brief Walk `argv` against `root`, collecting the resolved subcommand
/// path tokens.
///
/// Stops at the first token that is not a subcommand of the current node.
/// Flag-shaped tokens are skipped, consuming a following value token when
/// the flag takes one, so `--title plan` cannot be mistaken for the `plan`
/// subcommand. An unknown flag is assumed to take a value (conservative:
/// the parser will reject it properly a moment later).
///
/// Exposed for its own tests — it is pure argv-and-tree work with no
/// filesystem or environment access, and it is where a subtle mistake
/// would silently un-gate a verb.
/// @param root The command tree.
/// @param argv The full argv, INCLUDING argv[0].
/// @return The resolved path tokens, possibly empty.
export auto resolve_verb_path(const CLI::App& root, std::span<const std::string> argv) -> std::vector<std::string>;

/// @brief True when argv requests help before any `--` terminator.
///
/// Help renders a static usage tree with no database access, so it is
/// exempt from the gate for every verb regardless of classification.
/// Without the exemption `plan --help` refuses with exit 8, which also
/// breaks introspection tooling that loops `planar <verb> --help` from a
/// worktree checkout.
/// @param argv The full argv.
/// @return True when help was requested.
export auto argv_requests_help(std::span<const std::string> argv) -> bool;

/// @brief Render the four-line refusal block.
///
/// Separated from `check` so the exact wording is assertable without a
/// subprocess and without constructing a worktree.
/// @param verb The composed verb path, space-joined, e.g. `"plan create"`.
/// @param worktree_root The worktree the operator is standing in.
/// @param parent_repo_root The repository it was branched from.
/// @return The block, newline-terminated.
export auto refusal_message(std::string_view verb, std::string_view worktree_root, std::string_view parent_repo_root)
    -> std::string;

} // namespace planar::cmd::worktree_gate
