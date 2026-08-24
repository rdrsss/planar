/// @file verb_classification.cppm
/// @brief `planar.cmd.planar.verb_classification` — the single source of
/// truth for the worktree gate's planning-vs-execution policy (plan 996,
/// task 6137).
///
/// Behavior-preserving port of zig/src/cmd/planar/verb_classification.zig.
/// Three of that module's design notes are load-bearing and carried over
/// verbatim in behavior:
///
///   * **Centralized, not a per-node tag.** Adding a verb in a separate
///     change must not require remembering to tag its `CLI::App`. One file
///     keeps the policy auditable and puts it in front of a reviewer's eyes
///     when a new verb lands.
///   * **Unknown verbs default to `planning`, i.e. REFUSED.** The worst
///     case for a misclassified read is operator-recoverable ("cd to the
///     parent checkout and re-run"). The opposite default would silently
///     let a new planning verb through the gate, which is the exact failure
///     mode task 6137 exists to close.
///   * **Matching on resolved path tokens**, not on raw argv, so aliases
///     and flag values cannot be mistaken for subcommands.
module;

export module planar.cmd.planar.verb_classification;

import std;

namespace planar::cmd {

/// @brief The two policy buckets. The gate refuses `planning` from inside
/// a worktree; `execution_or_read` is always allowed.
export enum class verb_class : std::uint8_t {
  /// @brief Refused from a worktree cwd. Planning entity mutations, and
  /// `task done` (coders must use `planar-agent complete` instead).
  planning,
  /// @brief Allowed from a worktree cwd. Reads, the resume/handoff loop,
  /// the audit / capture / sync / workbench / workspace family, the
  /// orchestrator's read side, and every `* show` / `* list` leaf.
  execution_or_read,
};

/// @brief Classify a resolved verb path, e.g. `{"plan", "create"}`.
///
/// The matrix mirrors the tech spec's verb-classification table; see the
/// Zig original's doc comment for the full enumeration. Anything not
/// listed classifies as `planning`.
/// @param path The resolved subcommand path tokens, in order.
/// @return The policy bucket.
export auto classify(std::span<const std::string> path) -> verb_class;

} // namespace planar::cmd
