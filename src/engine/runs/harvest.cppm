/// @file harvest.cppm
/// @brief `planar.engine.runs.harvest` — ground-truth touch harvesting for
/// `bench harvest` (plan 996, task 6362).
///
/// Behavior-preserving port (D2) of zig/src/engine/runs/harvest.zig. Shells
/// `git diff --name-only` (via the layer-1 `planar.git` seam) against a
/// worktree, de-duplicates the resulting path set, and writes one
/// `run_touches kind='actual'` row per DISTINCT path through
/// `lifecycle::touch_idempotent` — the same `insert or ignore` primitive
/// this bucket had already ported and named as harvest's future entry
/// point (see lifecycle.cppm's cut list, now closed).
///
/// ## Two diff shapes, and a THIRD query hiding inside the first
///
/// `diff_spec` mirrors the oracle's `DiffSpec` union exactly:
///
///   - unset (`working_tree`): `git diff --name-only HEAD` — staged AND
///     unstaged edits to TRACKED files — merged with a SECOND query,
///     `git ls-files --others --exclude-standard`, which is the only way
///     to see a brand-new file that has never been staged. The oracle
///     added this second query at its own task 4289 after discovering
///     `git diff --name-only HEAD` alone misses untracked files; skipping
///     it here would silently under-report every harvest a coder cycle
///     creates a new file in. Reproduced faithfully, not simplified away.
///   - set (`range{base, head}`): `git diff --name-only base..head` alone.
///     A committed range already contains every new file the range
///     introduced, so no `ls-files` pass is needed or run.
///
/// ## DISTINCT, twice over
///
/// `diff_paths` de-duplicates ACROSS both queries in working-tree mode (a
/// path reported by both `diff` and `ls-files` — which cannot happen in
/// practice since one lists tracked and the other untracked paths, but the
/// oracle's merge function still guards it) and within either query alone
/// (a path git repeats is folded to one). `harvest` then writes through
/// `touch_idempotent`, which is ALSO an `insert or ignore` — so the
/// DISTINCT guarantee is enforced twice: once in memory before any SQL
/// runs, and once more at the UNIQUE constraint if the in-memory pass were
/// ever bypassed. The returned count is `diff_paths().size()`, matching
/// the oracle's own count (paths in the diff, not rows newly written —
/// re-harvesting an already-recorded path still counts it).
module;

export module planar.engine.runs.harvest;

import std;
import planar.db;
import planar.engine.runs.lifecycle;

namespace planar::engine::runs::harvest {

/// @brief A committed `base..head` range, the paired-flag arm of
/// `diff_spec`.
export struct diff_range {
  std::string base; ///< The `--base` sha/ref.
  std::string head; ///< The `--head` sha/ref.
};

/// @brief What `git diff` is computed against. Unset means `working_tree`
/// (the worktree/index vs HEAD, plus untracked files); set means the
/// committed `base..head` range. Mirrors the oracle's `DiffSpec` union —
/// see this module's header for why the two shapes run different queries.
export using diff_spec = std::optional<diff_range>;

/// @brief Why a harvest failed.
export enum class harvest_error {
  git_failed,  ///< git was not runnable, exited non-zero, or `dir` is not a repository.
  query_failed ///< The `run_touches` write failed for a reason other than a duplicate tuple.
};

/// @brief Arguments to `harvest`. Mirrors the oracle's `HarvestArgs`.
export struct harvest_args {
  std::filesystem::path worktree;    ///< The git worktree to diff.
  std::int64_t          run_id  = 0; ///< The owning run.
  std::int64_t          task_id = 0; ///< The task every written row is tagged with.
  diff_spec             spec;        ///< Working-tree (unset) or a committed range.
};

/// @brief Split newline-delimited raw text (one `git` query's stdout) into
/// a de-duplicated, order-stable list of paths: blank lines dropped, each
/// line trimmed of ` \t\r`, first occurrence wins.
///
/// Exposed for its own tests — pure string work, no subprocess, no
/// filesystem — mirroring the oracle's own separately-tested `parsePaths`.
/// This is what pins the DISTINCT guarantee: a path appearing twice in one
/// blob of raw `git diff --name-only` output must appear once here, which
/// is what makes `harvest`'s per-path `touch_idempotent` call run exactly
/// once per distinct path rather than once per line.
/// @param raw One query's raw, newline-delimited stdout.
/// @return The distinct paths, in first-seen order.
export auto parse_paths(std::string_view raw) -> std::vector<std::string>;

/// @brief Run `git diff --name-only` (plus, in working-tree mode, `git
/// ls-files --others --exclude-standard`) against `worktree` per `spec`,
/// and return the de-duplicated, order-stable set of changed paths.
///
/// Exposed separately from `harvest` so a caller — or a test — can inspect
/// the path set without writing any row, mirroring the oracle's own split
/// between `diffPaths` and `harvest`.
/// @param worktree The git worktree to diff.
/// @param spec Working-tree (unset) or a committed range.
/// @return The distinct paths, in first-seen order, or `git_failed`.
export auto diff_paths(const std::filesystem::path& worktree, const diff_spec& spec)
    -> std::expected<std::vector<std::string>, harvest_error>;

/// @brief Diff `args.worktree` per `args.spec` and insert one
/// `run_touches kind='actual'` row per distinct path via
/// `lifecycle::touch_idempotent`.
///
/// Idempotent: re-harvesting the same (run, task) is a safe no-op — rows
/// already present for the (run_id, task_id, path, kind) tuple are
/// silently skipped. The returned count is the number of distinct paths in
/// the diff, INCLUDING any whose row already existed — not the count of
/// newly-inserted rows. An empty diff returns zero with no error.
/// @param conn An open, migrated database connection.
/// @param args The worktree, run, task, and diff shape.
/// @return The count of distinct paths diffed, or the failure.
export auto harvest(db::connection& conn, const harvest_args& args) -> std::expected<std::size_t, harvest_error>;

} // namespace planar::engine::runs::harvest
