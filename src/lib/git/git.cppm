/// @file git.cppm
/// @brief `planar.git` — the layer-1 git-subprocess seam (plan 996, tasks
/// 6128 and 6137).
///
/// ## Why this module exists
///
/// Three separate ports had already grown their own private copy of "shell
/// `git -C <dir> <args>`, trim, treat any non-zero exit as no answer":
/// `src/cmd/planar/handlers/init.cpp`'s `probe_git_origin`, and
/// `src/cmd/planar-agent/locality.cpp`'s `run_git` / `run_git_trim`. A
/// third and fourth consumer arrived together — `capture session`'s
/// start-context probe (task 6128) and the worktree gate's `detect_worktree`
/// (task 6137) — and the fourth one is NOT in a `cmd_*` binary: worktree
/// detection belongs in `planar.engine.identity.scope`, which is layer 2
/// and therefore cannot reach into either `cmd_*` copy (D15/D18). So the
/// seam has to live at layer 1 whether or not the existing duplication is
/// consolidated; consolidating it is then free, and both former copies are
/// now thin wrappers over the functions below.
///
/// The absence of this seam is what four separate cycles kept running into.
/// It was named as a deferral in `planar.cmd.planar.scope`'s header, in
/// `planar.engine.runtime.capture`'s header, and on tasks 6105/6128/6137.
/// This module closes it.
///
/// ## The contract: "no answer", never a wrong answer
///
/// Every probe here is best-effort and total. `git` not installed, `dir`
/// not a repository, a signal, an empty stdout — all collapse to
/// `std::nullopt`. That is not laziness; it is the oracle's own behavior:
/// zig's `gitTrim` (zig/src/engine/runtime/capture.zig:242) switches on
/// `result.term` and returns null for every arm that is not `.exited` with
/// code 0, then returns null again when the trimmed stdout is empty. An
/// empty stdout is deliberately NOT an empty-string answer — see
/// `probe_origin_url`'s note, where storing `""` instead of NULL was a real
/// divergence the oracle avoids.
///
/// Stderr is routed to `/dev/null` for the same reason both former copies
/// did it: these are probes run underneath a verb, and git's "not a git
/// repository" complaint would otherwise interleave with the verb's real
/// output on the operator's terminal. The oracle captures and discards it.
///
/// ## Why `popen` and not `posix_spawn`
///
/// `popen` is what both pre-existing copies used, it is what the two
/// consumers' behavior was measured against, and every argument that
/// reaches the shell goes through `shell_quote` below. There is no
/// caller-supplied data in any argument list in this tree — the
/// subcommands are literals in `git.cpp` and the only interpolated values
/// are filesystem paths — but the quoting is unconditional so that
/// property does not have to be re-verified per call site.
module;

export module planar.git;

import std;

namespace planar::git {

/// @brief Run `git -C <dir> <args...>` and return its raw stdout on exit 0.
///
/// The primitive every other function here is built on. Exposed because
/// `git status --porcelain` needs the untrimmed, multi-line output, and
/// (since plan 996 task 6358) because `sessioncommits.cppm`'s `-z`
/// git-log/show output embeds NUL bytes as field separators. `run` is
/// BINARY-SAFE — embedded NULs survive intact, byte for byte, exactly as
/// they do through the oracle's `std.process.run` — not merely stdout up
/// to the first one; see git.cpp's implementation note for the bug this
/// closed and git.t.cpp's `-z`-labeled cases for the regression coverage.
/// @param dir The `-C` directory.
/// @param args The git subcommand and its arguments, in order.
/// @return The raw stdout, or unset when git could not be spawned, exited
/// non-zero, or died on a signal.
export auto run(const std::filesystem::path& dir, std::span<const std::string_view> args) -> std::optional<std::string>;

/// @brief `run`, with leading/trailing ASCII whitespace stripped and an
/// empty result reported as no answer.
///
/// Empty-is-no-answer is load-bearing, not a convenience: see this file's
/// header.
/// @param dir The `-C` directory.
/// @param args The git subcommand and its arguments, in order.
/// @return The trimmed stdout, or unset.
export auto run_trimmed(const std::filesystem::path& dir, std::span<const std::string_view> args) -> std::optional<std::string>;

/// @brief The git context `capture session` stamps onto a fresh session row.
///
/// Mirrors zig's `StartGitContext` (zig/src/engine/runtime/capture.zig:213).
export struct start_context {
  std::string repo_root;         ///< `git rev-parse --show-toplevel`, trimmed.
  std::string head_sha_at_start; ///< `git rev-parse HEAD`, trimmed.
};

/// @brief Probe `dir` for the pair `capture session` stamps.
///
/// Port of zig's `probeStartGitContext`. BOTH probes must answer: the Zig
/// original returns null the moment either `rev-parse --show-toplevel` or
/// `rev-parse HEAD` fails, so a repository with no commits yet (toplevel
/// answers, `HEAD` does not) yields no context at all rather than a
/// half-filled one. The session row's two columns are written together or
/// not at all.
/// @param dir The directory to probe, normally the operator's cwd.
/// @return Both values, or unset.
export auto probe_start_context(const std::filesystem::path& dir) -> std::optional<start_context>;

/// @brief Probe `dir` for its `origin` remote URL.
///
/// Port of the probe `planar init` runs to fill `projects.git_remote`.
/// Empty stdout reports unset, NOT the empty string: the oracle rejects it
/// after the exit-status check, so `git` answering with a blank line stores
/// SQL NULL.
/// @param dir The directory to probe.
/// @return The trimmed remote URL, or unset.
export auto probe_origin_url(const std::filesystem::path& dir) -> std::optional<std::string>;

/// @brief Result of the worktree-detection probe. Mirrors zig's
/// `WorktreeDetection` (zig/src/engine/identity/scope.zig:68).
export struct worktree_detection {
  bool                       is_worktree = false; ///< True when `dir` lives inside a SECONDARY worktree.
  std::optional<std::string> worktree_root;       ///< The worktree's working-tree root. Set only when `is_worktree`.
  std::optional<std::string> parent_repo_root;    ///< The repository the worktree was branched from. Set only when `is_worktree`.
};

/// @brief Classify `dir` as inside a secondary git worktree or not.
///
/// Behavior-preserving port of zig's `detectWorktree`. Two levels, in
/// order, and the ORDER is the contract:
///
///   1. **Fast path, no subprocess**: a `/.worktrees/<segment>/` boundary
///      anywhere in the path. This is the orchestrator's own convention,
///      and it classifies without git even being installed. `worktree_root`
///      is the path through the FIRST segment under `.worktrees/`;
///      `parent_repo_root` is the directory containing `.worktrees/`.
///   2. **Authoritative fallback**: one `git rev-parse
///      --path-format=absolute --show-toplevel --git-common-dir
///      --absolute-git-dir` invocation. `dir` is a secondary worktree if
///      and only if the per-worktree git dir differs from the repository's
///      common dir.
///
/// Two subtleties the Zig original documents at length, preserved verbatim
/// in behavior because both were real bugs it had to fix:
///
///   * `--path-format=absolute` is REQUIRED. Without it `--git-common-dir`
///     comes back relative to the INVOCATION cwd (not to `--show-toplevel`),
///     and resolving it against the toplevel misclassified any ordinary
///     checkout probed from two-or-more levels down as a secondary worktree.
///   * The discriminator is `--absolute-git-dir` vs `--git-common-dir`, NOT
///     "common dir equals `<toplevel>/.git`". A SUBMODULE checkout's common
///     dir is `<superproject>/.git/modules/<path>`, never `<toplevel>/.git`,
///     so the older rule reported every submodule as a secondary worktree.
///     A submodule's git dir and common dir are the same directory, so the
///     current rule classifies it correctly as a primary checkout.
///
/// Total, like everything else here: any failure reports "not a worktree".
/// A detection failure must never refuse an operator's planning verb.
/// @param dir The directory to classify, normally the operator's cwd.
/// @return The classification.
export auto detect_worktree(const std::filesystem::path& dir) -> worktree_detection;

/// @brief The fast path of `detect_worktree`, exposed for its own tests.
///
/// Returns the slice of `path` up to and including the segment immediately
/// after a `/.worktrees/` boundary. Pure string work — no filesystem, no
/// subprocess — so it is testable without constructing a repository, which
/// is exactly why it is separable.
/// @param path The path to scan.
/// @return The worktree root, or unset when there is no `/.worktrees/<seg>`
/// boundary.
export auto fast_path_worktree_root(std::string_view path) -> std::optional<std::string_view>;

} // namespace planar::git
