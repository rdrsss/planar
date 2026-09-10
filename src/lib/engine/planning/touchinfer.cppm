/// @file touchinfer.cppm
/// @brief `planar.engine.planning.touchinfer` — propose path-level task
/// touches from a task's own text, resolved against the repo tree (plan 996,
/// task 6330).
///
/// Behavior-preserving port (D2) of zig/src/engine/planning/touchinfer.zig.
/// The layer-3 half (repo resolution, `--apply`, rendering) lives in
/// `cmd/planar/handlers/task.cpp`, matching the Zig split.
///
/// ## What it is — and what the dispatch table used to claim it was
///
/// `dispatch.cpp` carried this leaf's deferral note as "773 lines of
/// git-diff and language-aware path inference". Both halves of that
/// description are WRONG, and were corrected in the same change that landed
/// this module. `touchinfer.zig` shells nothing, imports no git, and knows
/// no languages. It splits the task's title / body / next_action on
/// whitespace, keeps the tokens that look path-shaped, and `stat()`s each
/// one against the repo checkout. That is the whole algorithm. The note was
/// a paraphrase that outlived contact with the file — the exact failure mode
/// this project's briefs warn about — so it is replaced rather than
/// preserved.
///
/// Because it shells nothing, this module is NOT affected by the Zig 0.16.0
/// `std.Io.Threaded.global_single_threaded.io()` defect that made
/// `closeout.cppm` diverge (every `std.process.run` on that handle fails
/// with `OutOfMemory`). `touchinfer.zig` uses the same handle, but only for
/// `openDir` / `openFile` / `iterate`, which work. Verified by RUNNING the
/// oracle, not by reading it: a `docs/` token expanded to three real files.
/// This port therefore has no git-related divergence at all.
///
/// ## The over-declaration bias (decision 906)
///
/// The two error directions are NOT symmetric:
///
///   - OVER-declaring costs throughput. The task serializes when it might
///     have run in parallel. Recoverable at any time.
///   - UNDER-declaring costs correctness. Two tasks are marked eligible,
///     fanned into separate worktrees, both edit the same file, and the
///     collision surfaces at fan-in — after both burned a full cycle.
///
/// So every ambiguity resolves WIDE: a directory token expands to its files,
/// a bare basename yields EVERY matching path rather than a guess, and a
/// token that cannot be placed is reported as `unresolved` rather than
/// dropped silently.
///
/// ## Proposing wide is not the same as writing wide
///
/// That bias governs what is PROPOSED. What gets WRITTEN is narrower: only
/// `resolved` writes by default. Measured 2026-08-05 over 46 open tasks in
/// six real plans, applying parallel-eligibility rule 2 offline:
///
///     nothing declared (baseline)   0 / 46 eligible
///     resolved-only                14 / 46
///     resolved + wide              13 / 46
///
/// Wide expansion bought zero additional eligible tasks and cost one, via
/// rule 2's drop-both-on-tie: an UNDECLARED task removes only itself from
/// the eligible set, an OVER-DECLARED one removes its peers too.
///
/// ## DIVERGENCE — expansion ORDER is sorted here, readdir order there
///
/// The one divergence in this module, and it is deliberate.
///
/// The oracle builds `directory` and `basename` path lists by walking
/// `dir.iterate()` and appending in the order the filesystem hands entries
/// back, recursing into a subdirectory at the point it is encountered. That
/// order is neither sorted nor stable across filesystems. Captured from the
/// oracle against a fixture holding exactly `docs/a.md`, `docs/b.md` and
/// `docs/sub/c.md`:
///
///     "paths":["docs/b.md","docs/sub/c.md","docs/a.md"]
///
/// There is no rule there to reproduce — it is APFS hash order, and the same
/// tree on another filesystem answers differently. Pinning those bytes would
/// pin the test machine, not the contract, so this port SORTS both lists
/// lexicographically and pins that instead.
///
/// The divergence is confined to presentation and cannot reach state:
///
///   - The `too_broad` cutoff is a COUNT test (`> max_directory_expansion`),
///     so order never decides which classification a token gets. No partial
///     list ever escapes a truncated walk — an over-limit walk returns
///     `too_broad` with NO paths at all.
///   - `written` counts the same paths either way, and `add_touch_path` is
///     `insert or ignore` on `unique(task_id, repo_id, path)`.
///   - `task touches list`, the verb that reads the rows back, orders by
///     `p.slug, ttp.path` in SQL. Its output was already sorted in the
///     oracle and is byte-identical here.
///
/// So what changes is the order of lines under `not written — review:` and
/// of strings inside a candidate's JSON `paths` array. `resolved` candidates
/// — the only ones written by default — hold exactly one path and are
/// unaffected.

module;

export module planar.engine.planning.touchinfer;

import std;
import planar.db;

namespace planar::engine::planning::touchinfer {

/// @brief Cap on how many files a single directory or basename token may
/// expand to.
///
/// A token naming a huge tree is reported as `too_broad` rather than
/// silently expanding to hundreds of rows — over-declaration is safe, but an
/// unreviewable preview is not.
export inline constexpr std::size_t k_max_directory_expansion = 64;

/// @brief Which field of the task produced a candidate.
///
/// Carried into the preview so the operator can judge the evidence, not just
/// the verdict.
export enum class evidence : std::uint8_t {
  title,       ///< The task's title.
  body,        ///< The task's body.
  next_action, ///< The task's next_action.
  citation,    ///< Reserved by the oracle; no producer reaches it.
};

/// @brief Render an evidence value as the oracle's wire token.
/// @param value The evidence value.
/// @return A static string, no terminator.
export auto to_text(evidence value) -> std::string_view;

/// @brief How a candidate token resolved against the repo tree.
export enum class classification : std::uint8_t {
  /// Exact repo-relative path of an existing file.
  resolved,
  /// Token named a non-empty directory; expanded to the files beneath it.
  directory,
  /// Bare basename matching one or more files anywhere in the tree. ALL
  /// matches are proposed (decision 906) — never a single guess.
  basename,
  /// Path-shaped but nothing in the tree matched. Also what an EXISTING but
  /// EMPTY directory classifies as — verified against the oracle, which
  /// reports `emptydir/` as `unresolved`, not as a zero-path `directory`.
  unresolved,
  /// A directory or basename whose expansion exceeded
  /// `k_max_directory_expansion`. Reported with NO paths so the operator can
  /// declare a narrower path deliberately.
  too_broad,
};

/// @brief Render a classification as the oracle's wire token.
/// @param value The classification.
/// @return A static string, no terminator.
export auto to_text(classification value) -> std::string_view;

/// @brief Whether a candidate of this classification contributes writable
/// rows under the given policy.
///
/// `resolved` — an exact path the task named — always writes. The wide
/// classifications write only when the caller opts in via `--wide`, because
/// measurement showed they cost eligibility rather than adding it.
/// `unresolved` and `too_broad` are preview-only under either policy.
/// @param value The classification.
/// @param wide Whether `--wide` was passed.
/// @return True when candidates of this classification write rows.
export auto is_writable(classification value, bool wide) -> bool;

/// @brief One token lifted from the task text, with how it resolved.
export struct candidate {
  std::string    token;                                        ///< The literal token as it appeared in the task text.
  evidence       evidence_       = evidence::title;            ///< Which field produced it.
  classification classification_ = classification::unresolved; ///< How it resolved.
  /// Repo-relative paths this candidate proposes, sorted (see the file
  /// header's DIVERGENCE note). Empty for `unresolved` and `too_broad`.
  std::vector<std::string> paths;
  std::int64_t             repo_id = 0; ///< The repo (`projects.id`) the paths are relative to.
};

/// @brief A whole task's inference result.
export struct inference {
  std::int64_t           task_id = 0; ///< The task the candidates came from.
  std::vector<candidate> candidates;  ///< In task field order: title, body, next_action.

  /// @brief Count of PATHS that would be written under the given policy.
  /// @param wide Whether `--wide` was passed.
  /// @return The number of rows `--apply` would attempt.
  [[nodiscard]] auto writable_count(bool wide) const -> std::size_t;

  /// @brief Count of CANDIDATES surfaced for review but not written.
  ///
  /// Note the asymmetry with `writable_count`, which is deliberate and is
  /// the oracle's: this counts candidates, that counts paths. A single
  /// `directory` candidate proposing three files contributes 1 to review and
  /// 3 to writable.
  /// @param wide Whether `--wide` was passed.
  /// @return The number of candidates held back.
  [[nodiscard]] auto review_count(bool wide) const -> std::size_t;
};

/// @brief Strip a trailing `:<line>` or `:<line>-<line>` reference.
///
/// Task prose cites locations as `docs/cli-reference.md:339` and
/// `src/engine/x.zig:12-40`; the path is the part before the colon. Without
/// this, the single most common way this codebase names a file resolves to
/// `unresolved` — a silent recall gap, which is the dangerous direction.
///
/// A colon at index 0, a trailing colon, or a suffix holding anything but
/// digits and `-` leaves the input untouched.
/// @param s The token.
/// @return A view into `s`.
export auto strip_line_suffix(std::string_view s) -> std::string_view;

/// @brief True when `s` looks like it could name a path worth resolving.
///
/// Deliberately permissive — a false candidate costs one `unresolved` row in
/// the preview, while a missed candidate costs a silent under-declaration.
/// Rejects the empty string, anything over 512 bytes, URLs (`://`), tokens
/// holding whitespace, absolute paths, and `../` traversal. Accepts anything
/// containing `/`; otherwise requires an interior extension dot, so a bare
/// `strategy.zig` is a candidate and a bare `README` or an English word is
/// not.
/// @param s The token.
/// @return True when the token should be resolved.
export auto is_path_shaped(std::string_view s) -> bool;

/// @brief Split `text` on whitespace and yield trimmed, path-shaped tokens.
///
/// Trims whitespace, backtick, single and double quote, the six bracket
/// characters, comma, semicolon and colon from both ends, then strips
/// trailing period characters from the RIGHT only (a leading dot is
/// meaningful in a `./x` form, a trailing one never is), then strips a
/// `:<line>` suffix. The exact set is `k_trim_chars` in the implementation;
/// it is spelled out in prose here because a literal containing a backtick
/// cannot be quoted in a doc comment without breaking the parser.
/// Duplicates are preserved here; `infer_from_text` dedupes across the whole
/// task.
/// @param text The field text.
/// @return The tokens, in order of appearance.
export auto extract_tokens(std::string_view text) -> std::vector<std::string>;

/// @brief Resolve one token against one repo root.
///
/// A trailing slash is normalized away first: `docs/` and `docs` name the
/// same directory. An existing file is `resolved`; an existing directory
/// expands recursively, skipping dot-entries, to `directory` (or `too_broad`
/// past the cap, or `unresolved` when it holds no non-hidden files). A token
/// that matched nothing and holds NO `/` is searched for as a bare basename
/// across the whole tree. Anything left is `unresolved`.
/// @param root Absolute path to the repo checkout.
/// @param token The token to resolve.
/// @return The classification and its sorted repo-relative paths.
export auto classify_token(const std::filesystem::path& root, std::string_view token)
    -> std::pair<classification, std::vector<std::string>>;

/// @brief The task text inference reads.
///
/// Split out so the resolution pass is testable without a database.
export struct task_text {
  std::string title;       ///< The task's title.
  std::string body;        ///< The task's body, `""` when NULL.
  std::string next_action; ///< The task's next_action, `""` when NULL.
};

/// @brief Resolve every path-shaped token in `text` against `root`.
///
/// Candidate order follows field order (title, body, next_action) so the
/// preview reads top-down like the task does. A token repeated across fields
/// yields ONE candidate, attributed to the first field that produced it.
/// An empty field is skipped entirely.
/// @param text The task's three text fields.
/// @param repo_id The repo the paths are relative to.
/// @param root Absolute path to the repo checkout.
/// @return The candidates, in first-appearance order.
export auto infer_from_text(const task_text& text, std::int64_t repo_id, const std::filesystem::path& root)
    -> std::vector<candidate>;

/// @brief Why `infer` failed.
export enum class infer_error : std::uint8_t {
  not_found,    ///< No task with that id.
  query_failed, ///< SQLite refused.
};

/// @brief Read a task's text from the database and infer its touches.
///
/// Proposes only — no `task_touch_paths` row is written here. The caller
/// previews, and writes on explicit confirmation.
/// @param conn The open database connection.
/// @param task_id The task to read.
/// @param repo_id The repo the paths are relative to.
/// @param root Absolute path to the repo checkout.
/// @return The inference, or `not_found` / `query_failed`.
export auto infer(db::connection& conn, std::int64_t task_id, std::int64_t repo_id, const std::filesystem::path& root)
    -> std::expected<inference, infer_error>;

} // namespace planar::engine::planning::touchinfer
