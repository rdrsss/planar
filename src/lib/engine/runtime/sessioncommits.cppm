/// @file sessioncommits.cppm
/// @brief `planar.engine.runtime.sessioncommits` — the read half PLUS the
/// git-walk write half zig/src/engine/runtime/sessioncommits.zig's
/// `capture commits` leaf needs (plan 996, tasks 6262 and 6358).
///
/// ## WHAT TASK 6358 ADDED, AND WHAT IS STILL NOT HERE
///
/// The Zig original is 1205 lines; this module is still well short of that,
/// and the gap is named rather than implied. Task 6358 added exactly the
/// slice `capture commits` reaches, now that `planar.git` (task 6128/6137)
/// closes the process-spawn seam this module used to be blocked on:
///
///   - `resolve_repo_root_strict` — port of `resolveRepoRootStrict`.
///   - `walk_strict` — port of `walkStrict`.
///   - `resolve_shas` — port of `resolveShas`.
///   - `record_count` — port of `recordCount`, the WRITE half.
///   - `commit_meta` — port of the Zig `Commit` struct these three return.
///
/// Still absent, because nothing this tree ports reaches them: `walk` (the
/// FAIL-SOFT variant `close_session`'s automatic `recordSessionWindow`
/// would use), `walkClaimWindow` and `recordClaimWindowBestEffort` (the
/// `planar-agent` terminal-verb claim-window fold). `capture commits` is
/// the OPERATOR path and calls only the strict functions above — see
/// zig's `recordCommits` (capture.zig:163), which never calls `walk`.
/// Wiring the automatic fail-soft capture into `capture end` or the
/// terminal verbs is a SEPARATE port, not a side effect of this one; see
/// this module's CMakeLists.txt for the tracking note.
///
/// The pre-existing READ half is unchanged:
///
///   - `list_for_sessions` — `select … from session_commits where
///     session_id in (…)`, for `audit trail`'s commits fold-in.
///   - `list_filtered` — the same table under an optional session and/or
///     task predicate, the task arm joining `agent_work_claims`. Also
///     pure SQL.
///   - `render_json` / `render_json_list` — the persisted-row JSON shape.
///
/// ## THE "GIT WALK BLOCKS `audit commits`" CLAIM WAS WRONG, AND IT COST A
/// ## MILESTONE
///
/// This header, `handlers/audit.cppm` and `dispatch.cpp` all used to say
/// `audit commits` was blocked on the walk — "it is about walking git, not
/// reading rows". That is false and task 6272 corrected it. Read the
/// oracle handler (zig/src/cmd/planar/handlers/audit/commits.zig, 78
/// lines): it calls `session.getById`, `task.show`, `listFiltered`,
/// `writeJsonList`, and a local text table. It NEVER calls `walk`,
/// `walkStrict`, `resolveShas` or anything else that spawns a process. The
/// leaf reads rows some EARLIER `capture commits` run wrote; walking git is
/// what fills the table, not what queries it.
///
/// `capture commits` and `bench harvest` really are blocked on the walk.
/// `audit commits` never was. The correction is recorded here rather than
/// quietly deleted because the wrong claim survived three files and two
/// tasks, and the shape of the mistake — inferring a leaf's dependencies
/// from its MODULE's rather than from its own handler — is the reusable
/// part.
///
/// The read path was originally carved out because `audit trail` needs it
/// and needs nothing else. The oracle files all of this in this same
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

/// @brief Why a read OR write failed. Mirrors zig's `sessioncommits.Error`
/// union (`GitFailed`, `NotGit`, `MalformedOutput`, `QueryFailed`) — the
/// three new members below are task 6358's, added for the git-walk write
/// half; `query_failed` alone was sufficient while this module was
/// read-only.
export enum class commits_error : std::uint8_t {
  query_failed,     ///< An underlying SQL statement failed.
  git_failed,       ///< A git subprocess could not be spawned, exited non-zero, or died on a signal.
  not_git,          ///< `resolve_repo_root_strict`'s directory is not inside a git repository.
  malformed_output, ///< git's `-z`-delimited stdout did not carry the expected field count.
};

/// @brief One commit's metadata as read directly from git, before it is
/// persisted. Mirrors zig's `Commit` (sessioncommits.zig:12).
export struct commit_meta {
  std::string                sha;          ///< The commit sha, verbatim.
  std::optional<std::string> repo_root;    ///< The checkout the walk ran in.
  std::optional<std::string> branch;       ///< The branch at walk time.
  std::optional<std::string> subject;      ///< The commit subject.
  std::optional<std::string> author;       ///< The commit author.
  std::optional<std::string> committed_at; ///< The commit's own timestamp, ISO 8601.
};

/// @brief Resolve `dir` to its git toplevel, strictly.
///
/// Port of zig's `resolveRepoRootStrict`. The Zig original converts EVERY
/// underlying `GitFailed` into `NotGit` here — not installed, not a
/// repository, and a genuine transient git failure all collapse to the
/// same answer — and this preserves that collapse rather than
/// distinguishing the causes the oracle does not distinguish either.
/// @param dir The directory to resolve.
/// @return The absolute toplevel path (trimmed), or `not_git` /
/// `malformed_output` (an exit-0 answer that trims to empty — unreached in
/// practice since `rev-parse --show-toplevel` never succeeds with blank
/// output, kept because the oracle has the same guard).
export auto resolve_repo_root_strict(const std::filesystem::path& dir) -> std::expected<std::string, commits_error>;

/// @brief Walk `base_sha..HEAD` in `dir` and return each commit's metadata,
/// newest first, surfacing every git/parse failure. Port of zig's
/// `walkStrict`.
///
/// AN EMPTY, EXIT-0 RESULT IS SUCCESS WITH AN EMPTY VECTOR, NOT AN ERROR.
/// `git log` on a valid range with no commits in it answers with empty
/// stdout at exit 0 — this is the "no new commits" acceptance path, and
/// collapsing it into `git_failed` would refuse the operator's own
/// no-op `capture commits --since HEAD`.
/// @param dir The `-C` directory.
/// @param base_sha The range's exclusive lower bound.
/// @param repo_root The value stamped onto every returned commit's
/// `repo_root`; when unset, `dir` itself is used (mirrors the oracle's
/// `args.repoRoot orelse args.dir`).
/// @return The commits, newest first, or a `commits_error`.
export auto walk_strict(const std::filesystem::path& dir, std::string_view base_sha,
                        std::optional<std::string_view> repo_root = std::nullopt)
    -> std::expected<std::vector<commit_meta>, commits_error>;

/// @brief Resolve explicit SHAs to the same metadata shape `walk_strict`
/// returns, in the REQUESTED order. Port of zig's `resolveShas`.
///
/// An empty `shas` returns an empty vector without spawning git — mirrors
/// the oracle's own early return.
/// @param dir The `-C` directory.
/// @param shas The SHAs to resolve, in the order they should appear in the
/// result.
/// @param repo_root As `walk_strict`.
/// @return The commits, in `shas`' order, or a `commits_error`.
export auto resolve_shas(const std::filesystem::path& dir, std::span<const std::string_view> shas,
                         std::optional<std::string_view> repo_root = std::nullopt)
    -> std::expected<std::vector<commit_meta>, commits_error>;

/// @brief Persist `commits` for `session_id`, returning how many rows were
/// NEWLY inserted. Port of zig's `recordCount`.
///
/// The `(session_id, sha)` unique constraint (`insert or ignore`) makes
/// re-recording idempotent by design: a duplicate row counts as zero, not
/// a failure. `record_count` rather than a void `record` because
/// `capture commits`' own stdout reports the new-row count.
/// @param conn An open, migrated database connection.
/// @param session_id The owning session.
/// @param claim_id The claim window the commits fell under, or unset —
/// `capture commits` always passes unset; the claim-window fold is a
/// `planar-agent` terminal-verb concern this port does not reach.
/// @param commits The commit metadata to persist.
/// @return The count of newly inserted rows, or `commits_error::query_failed`.
export auto record_count(db::connection& conn, std::int64_t session_id, std::optional<std::int64_t> claim_id,
                         std::span<const commit_meta> commits) -> std::expected<std::size_t, commits_error>;

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

/// @brief The `list_filtered` predicate. Mirrors zig's `ListFilter`.
///
/// Both fields unset is a legal, meaningful state: it lists EVERY row in
/// `session_commits`, which is what `audit commits` with no flags answers.
/// It is not a "no filter given, refuse" case.
export struct list_filter {
  std::optional<std::int64_t> session_id; ///< Restrict to one session.
  std::optional<std::int64_t> task_id;    ///< Restrict to commits under a claim on this task.
};

/// @brief Every `session_commits` row matching `filter`, newest RECORDED
/// first (plan 996, task 6277).
///
/// ## THE TASK ARM IS A JOIN, NOT A COLUMN
///
/// `session_commits` has no `task_id`. The task predicate reaches the task
/// through `claim_id` -> `agent_work_claims`, matching `entity_kind =
/// 'task' and entity_id = ?`. Two consequences that a column-shaped mental
/// model gets wrong:
///
///   - The join is INNER, so filtering by task silently drops every row
///     with a NULL `claim_id` — commits recorded outside any claim window.
///     They are real rows and `--session` (or no filter) still lists them.
///   - `entity_kind = 'task'` is part of the predicate, not decoration. A
///     claim on some other entity kind that happens to share the numeric
///     id must not match.
///
/// The join is added ONLY when the task filter is set, so a session-only
/// or unfiltered listing keeps the NULL-claim rows.
///
/// THE `join` KEYWORD ITSELF IS NOT INDEPENDENTLY OBSERVABLE, and saying so
/// is the point — the second measured survivor in this module. Rewriting it
/// as `left join` changes NOTHING: the `where` clause constrains
/// `awc.entity_kind` and `awc.entity_id`, and a row whose right side is all
/// NULL fails both, so the outer join collapses back to an inner one. The
/// break-probe for that mutation SURVIVES, and it is a genuinely equivalent
/// mutant rather than a gap in the tests. What IS pinned, and what the
/// probes kill, is the observable behaviour underneath: widening
/// `entity_id = ?` or pointing `entity_kind` at another kind both change
/// the answer and both fail immediately.
///
/// ## NO `limit`, DELIBERATELY
///
/// `list_for_sessions` takes one and this does not, because the oracle's
/// two functions differ the same way — `audit commits` prints everything
/// it matches. Adding a cap here would be a behaviour change disguised as
/// a signature tidy-up.
///
/// The sort is `recorded_at desc, id desc`: newest-RECORDED, not
/// newest-COMMITTED, while the renderer DISPLAYS `committed_at`. See this
/// module's header.
/// @param conn An open, migrated database connection.
/// @param filter The predicate; a default-constructed one lists everything.
/// @return The rows, or `commits_error::query_failed`.
export auto list_filtered(db::connection& conn, const list_filter& filter)
    -> std::expected<std::vector<commit_row>, commits_error>;

/// @brief Render one row as the single-line JSON object.
///
/// Hand-assembled by the oracle rather than stringified from the struct,
/// and the difference is observable: `claim_id` and the five optional
/// strings each render an explicit `null` when unset — the keys are always
/// present, never omitted. Field order is `commit_row`'s declaration
/// order, which is the oracle's `Row` order and the `select` list's order.
///
/// String values ARE escaped here (the oracle routes them through
/// `std.json.Stringify.encodeJsonString`), unlike the hand-rolled
/// `assoc add` / `assoc remove` success literals which are not. The two
/// look like the same pattern and are not; a commit subject containing a
/// quote is ordinary, so this one had to be checked rather than assumed.
/// @param row The row to render.
/// @return The JSON object with NO trailing newline.
export auto render_json(const commit_row& row) -> std::string;

/// @brief Render rows as the single-line JSON array.
///
/// The empty list renders `[]`. `audit commits --json` over a database
/// with no commits is exit 0 and `[]`, not a refusal and not an empty
/// line.
/// @param rows The rows, in the order given.
/// @return The JSON array with NO trailing newline — a fragment the caller
/// terminates.
export auto render_json_list(std::span<const commit_row> rows) -> std::string;

} // namespace planar::engine::runtime::sessioncommits
