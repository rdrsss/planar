/// @file sessioncommits.cppm
/// @brief `planar.engine.runtime.sessioncommits` — the READ half of
/// zig/src/engine/runtime/sessioncommits.zig (plan 996, task 6262).
///
/// ## THIS MODULE IS DELIBERATELY A FRACTION OF ITS ORACLE, AND THE
/// ## FRACTION IS NAMED
///
/// The Zig original is 1205 lines and this is a hundred-odd, so a reader
/// who sees the module name and concludes `sessioncommits` is ported would
/// be wrong. Exactly ONE function is here:
///
///   - `list_for_sessions` — `select … from session_commits where
///     session_id in (…)`. Pure SQL over a table this tree already
///     migrates. THAT IS ALL.
///
/// Everything else in the original is a `git` subprocess walk —
/// `walk` / `walkStrict` / `walkClaimWindow` / `recordClaimWindowBestEffort`
/// and the ref-resolution and per-commit-metadata machinery under them.
/// Those need the process-spawn seam this tree does not have, and they are
/// the reason `capture commits`, `audit commits`, `bench harvest` and the
/// four `planar-agent` terminal verbs' claim-window fold are all still
/// deferred. NOTHING here unblocks any of them.
///
/// The read path is carved out because `audit trail` needs it and needs
/// nothing else: the leaf renders a "commits:" section out of rows already
/// in the table, and would otherwise have to reach into `session_commits`
/// with handler-local SQL. The oracle files this same function in this same
/// module, so the split follows its layering rather than inventing one.
///
/// ## THE EMPTY-`session_ids` GUARD IS NOT LOAD-BEARING, AND SAYING SO IS
/// ## THE POINT
///
/// An empty `session_ids` returns an empty vector without touching the
/// database. The obvious justification — "the composed `in ()` would not be
/// legal SQL" — IS FALSE, and was written here before being checked. SQLite
/// accepts an empty `IN ()` list as a documented extension and evaluates it
/// to false:
///
///     sqlite> select count(*) from t where a in ();
///     0
///
/// So removing the guard changes NO observable behaviour, and a break-probe
/// that deletes it SURVIVES. That was measured, not assumed. The guard is
/// kept anyway for two honest reasons — the oracle has the same early
/// return, and depending on a non-standard SQLite extension in a query this
/// module composes by hand is a needless bet — but no test here claims to
/// kill it, and none should. What IS pinned is the observable contract:
/// empty in, empty out, and NOT an error.
///
/// A non-empty `session_ids` matching no row also returns empty, having run
/// the query. Both are answers; neither is a failure.
///
/// ## THE ORDER IS `recorded_at desc, id desc` — NOT `committed_at`
///
/// Newest-RECORDED first, which is not the same as newest-COMMITTED: a
/// commit walked late but authored early sorts by when Planar saw it. The
/// renderer then DISPLAYS `committed_at` when present and falls back to
/// `recorded_at`, so a naive reading of the rendered output suggests it is
/// sorted by the column shown. It is not. Captured from the oracle with a
/// fixture whose two rows disagree on exactly this.
module;

export module planar.engine.runtime.sessioncommits;

import std;
import planar.db;

namespace planar::engine::runtime::sessioncommits {

/// @brief One `session_commits` row.
///
/// Field order mirrors the oracle's `Row` and the `select` list behind it,
/// so a renderer walking the struct emits the oracle's key order.
export struct commit_row {
  std::int64_t                id         = 0; ///< The `session_commits` row id.
  std::int64_t                session_id = 0; ///< The owning `sessions` row.
  std::optional<std::int64_t> claim_id;       ///< The claim the commit fell under, when known.
  std::string                 sha;            ///< The commit sha, verbatim.
  std::optional<std::string>  repo_root;      ///< The checkout the walk ran in.
  std::optional<std::string>  branch;         ///< The branch at walk time.
  std::optional<std::string>  subject;        ///< The commit subject.
  std::optional<std::string>  author;         ///< The commit author.
  std::optional<std::string>  committed_at;   ///< The commit's own timestamp.
  std::string                 recorded_at;    ///< When Planar recorded it.
};

/// @brief Why a read failed.
export enum class commits_error : std::uint8_t {
  query_failed, ///< An underlying SQL statement failed.
};

/// @brief Every `session_commits` row for any of `session_ids`, newest
/// RECORDED first, capped at `limit` when one is given.
///
/// See this module's header: the sort column is `recorded_at`, not the
/// `committed_at` the caller renders.
/// @param conn An open, migrated database connection.
/// @param session_ids The sessions to read. Empty returns empty without
/// running a query.
/// @param limit The row cap, or unset for no cap.
/// @return The rows, or `commits_error::query_failed`.
export auto list_for_sessions(db::connection& conn, std::span<const std::int64_t> session_ids, std::optional<std::int64_t> limit)
    -> std::expected<std::vector<commit_row>, commits_error>;

} // namespace planar::engine::runtime::sessioncommits
