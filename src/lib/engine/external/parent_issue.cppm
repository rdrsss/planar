/// @file parent_issue.cppm
/// @brief `planar.engine.external.parent_issue` — GitHub `parent-issue`
/// propagation strategy (plan 996, task 6353).
///
/// Behavior-preserving port (D2) of
/// `zig/src/engine/extsync/parent_issue.zig`, MINUS the two exports task
/// 6335 already carried out of that file into `planar.engine.external.link`:
/// `loadExistingMirror` (as `link::load_existing_mirror`) and `recordLink`
/// (as `link::record_mirror_link`). This module calls both rather than
/// redefining them.
///
/// ## Why this file lives in `engine_external` and not `engine_extsync`
///
/// `engine_extsync` (github.cppm, jira.cppm, propagate.cppm) deliberately
/// carries NO `db` edge — that is what makes every adapter test in that
/// bucket runnable with no database (see engine_extsync/CMakeLists.txt).
/// The oracle's `parent_issue.zig` is majority SQL: `childPlansOf`,
/// `tasksUnderPlan`, `directTasksOf`, `resolveTargetRepo`,
/// `entityForCreate`, the sub-issue-support cache read/write, and
/// `postDecisionComments` all issue queries directly. `engine_external`
/// already has the `db` edge (task 6106) and already carries this file's
/// two smallest exports (task 6335), so the rest joins them here rather
/// than opening a THIRD home for one oracle file. D18 (no `engine_*` ->
/// `engine_*` edges) is why this module never imports
/// `planar.engine.extsync.github` — see `gh_client` below.
///
/// ## `gh_client` is the type-erasure seam, same shape as `sync.cppm`'s
///
/// The oracle's `GhClient` is a Zig struct of function pointers so
/// `propagateParentIssueWithRepo` can be unit-tested with a `FakeClient`
/// and driven in production by a real `GithubAdapter`, with NEITHER side
/// naming the other. `sync.cppm` solved the identical problem — a
/// layer-2 bucket needing to call an adapter without depending on the
/// layer-2 peer that defines one — with an abstract base class over
/// `planar.adapter`'s interface; this module repeats that shape rather
/// than inventing a different one, because `planar.adapter`'s
/// `external_adapter` exposes only `validate`/`pull`/`push`/`render` and
/// none of the four GitHub REST operations this flow needs
/// (`probe`/`create_issue`/`link_sub_issue`/`post_comment`).
///
/// No production implementation of `gh_client` exists yet: `github_adapter`
/// (engine_extsync/github.cppm) has not grown `createIssue` /
/// `linkSubIssue` / `linkSubIssueProbe` / `postComment` — every one of
/// them is still listed as deferred-with-`ext-propagate` in that bucket's
/// CMakeLists.txt. Wiring a real `gh_client` over those (once they land)
/// plus the `ext propagate` CLI dispatch is follow-up work; see this
/// module's task-body note for the exact remaining pieces.
///
/// ## What is NOT here
///
/// `propagate.zig`'s `selectStrategy` (the ADR-0006 repo-count bucketing
/// that picks `github-zero-repo` / `github-parent-issue` /
/// `github-projects-v2`), `walkTree`/`freeTree` (already ported to
/// `planar.engine.planning.descendants`, task 6298) and `strategy.zig`
/// stay deferred with the `ext propagate` CLI leaf, which this module does
/// not attempt to unblock by itself. `projects_v2.zig` (the sibling
/// GitHub-specific file) is untouched — see this task's report for its
/// measured size.
module;

export module planar.engine.external.parent_issue;

import std;
import planar.db;
import planar.engine.external.link;

namespace planar::engine::external::parent_issue {

/// @brief The per-entity outcome the engine reports.
export enum class op : std::uint8_t {
  created,
  skipped,
  failed,
};

/// @brief One row in a `report`'s `results`.
export struct entity_result {
  std::string entity_kind;   ///< `"plan"` or `"task"`.
  std::int64_t entity_id = 0;
  std::string title;
  op          operation  = op::created;
  std::string external_id;   ///< `"owner/repo#N"`, or empty on `failed`.
  std::string external_url;  ///< Empty on `skipped`/`failed`.
  std::string error_name;    ///< Empty unless `operation == op::failed`.
};

/// @brief The propagate summary plus per-entity rows.
export struct report {
  std::size_t created = 0;
  std::size_t skipped = 0;
  std::size_t failed  = 0;
  /// @brief Effective strategy after fallback resolution. Always
  /// `"github-parent-issue"` for this module — the CLI layer decides
  /// whether a probe failure should fall back to a different strategy.
  std::string strategy = "github-parent-issue";
  std::vector<entity_result> results;
};

/// @brief The subset of the oracle's `PropagateOpts` the parent-issue path
/// needs.
export struct opts {
  std::int64_t    sys_id = 0;
  std::string_view sys_slug;
  bool            dry_run = false;
  link::sync_direction sync_direction_ = link::sync_direction::two_way;
};

/// @brief The wire result of a single REST issue create.
export struct created_issue {
  std::int64_t number = 0;
  std::string  node_id;
};

/// @brief Why a `gh_client` call failed.
///
/// Deliberately two members, mirroring the ONE distinction the oracle's
/// callers make: `probe` treats `not_found` (a 404) as "sub-issues
/// unsupported" and any OTHER error as "assume supported" (see
/// `detect_parent_issue_support`'s doc comment for why that asymmetry is
/// oracle behavior, not a bug this port introduces). Every other call site
/// (`create_issue`, `link_sub_issue`, `post_comment`) treats every error
/// the same regardless of which member it is.
export enum class gh_client_error : std::uint8_t {
  not_found,
  other,
};

/// @brief The GitHub REST surface this flow needs, type-erased so this
/// module never names `planar.engine.extsync.github` (D18). See this
/// file's header.
export class gh_client {
public:
  virtual ~gh_client() = default;

  /// @brief Probe whether the repo exposes the sub-issue REST endpoint.
  /// @param owner The repo owner.
  /// @param repo The repo name.
  /// @return Success when supported, `gh_client_error::not_found` when the
  /// probe 404s, or `gh_client_error::other` for any other transport
  /// failure.
  virtual auto probe(std::string_view owner, std::string_view repo) -> std::expected<void, gh_client_error> = 0;

  /// @brief Create a regular issue.
  /// @param owner The repo owner.
  /// @param repo The repo name.
  /// @param title The issue title.
  /// @param body The issue body.
  /// @param labels Labels to attach; always empty from this module today.
  /// @return The created issue, or the failure.
  virtual auto create_issue(std::string_view owner, std::string_view repo, std::string_view title, std::string_view body,
                            std::span<const std::string> labels) -> std::expected<created_issue, gh_client_error> = 0;

  /// @brief Link `child_number` as a sub-issue of `parent_number`.
  /// @param owner The repo owner.
  /// @param repo The repo name.
  /// @param parent_number The parent issue number.
  /// @param child_number The child issue number.
  /// @return Success, or the failure. A failure here is NON-FATAL to the
  /// caller — the issue was already created.
  virtual auto link_sub_issue(std::string_view owner, std::string_view repo, std::int64_t parent_number,
                              std::int64_t child_number) -> std::expected<void, gh_client_error> = 0;

  /// @brief Post a comment on the issue identified by `external_id`.
  /// @param external_id The `"owner/repo#N"` identifier.
  /// @param body The comment body.
  /// @return Success, or the failure. Non-fatal to the caller.
  virtual auto post_comment(std::string_view external_id, std::string_view body) -> std::expected<void, gh_client_error> = 0;
};

/// @brief Why a parent-issue operation failed. Mirrors the oracle's
/// `Error` union one-for-one (`WriteFailed` folds into `query_failed` here,
/// same as every other SQL-failure arm in this bucket).
export enum class parent_issue_error : std::uint8_t {
  no_touched_repos,
  cannot_resolve_repo,
  sub_issue_unsupported,
  query_failed,
  bad_config,
};

/// @brief One repo's GitHub coordinates.
export struct repo_coords {
  std::string owner;
  std::string repo;
};

/// @brief Extract `(owner, repo)` from a GitHub git-remote URL.
///
/// Recognizes `git@github.com:owner/repo(.git)?`,
/// `https://github.com/owner/repo(.git)?` and
/// `ssh://git@github.com/owner/repo(.git)?`, with any number of trailing
/// slashes tolerated. Anything else — including a non-GitHub host or a
/// path with more than one `/` after the owner — is unset.
/// @param git_remote The remote URL text.
/// @return The coordinates, or unset when unparseable.
export auto parse_github_repo(std::string_view git_remote) -> std::optional<repo_coords>;

/// @brief Resolve a `projects.id` to GitHub coordinates.
///
/// Prefers `projects.git_remote`, parsed via `parse_github_repo`; falls
/// back to the project's `slug` read as a literal `"owner/repo"` (the
/// shape every fixture project in this tree already uses).
/// @param conn An open, migrated connection.
/// @param project_id The project row id.
/// @return The coordinates, unset when the project has neither a usable
/// remote nor an `owner/repo`-shaped slug, or the query failure.
export auto project_github_coords(db::connection& conn, std::int64_t project_id)
    -> std::expected<std::optional<repo_coords>, parent_issue_error>;

/// @brief Resolve the single target repo for a single-repo feature.
///
/// Walks the same distinct-touched-repos set `ext propagate`'s strategy
/// selector consults and returns coordinates for the first repo.
/// @param conn An open, migrated connection.
/// @param anchor_plan_id The anchor plan.
/// @return The coordinates, `no_touched_repos` when the feature touches
/// none, `cannot_resolve_repo` when the first one can't be mapped to
/// GitHub coordinates, or the query failure.
export auto resolve_target_repo(db::connection& conn, std::int64_t anchor_plan_id)
    -> std::expected<repo_coords, parent_issue_error>;

/// @brief Probe (or read the cached result of probing) sub-issue REST
/// support for `owner/repo`, caching the outcome on the anchor's mirror
/// link's `config_json.sub_issue_supported`.
///
/// A cache hit short-circuits the probe entirely. On a miss, `client`'s
/// `probe` runs once: a `not_found` failure caches/returns `false`; ANY
/// OTHER outcome — success OR a non-404 failure — caches/returns `true`.
/// That asymmetry (treating a probe TRANSPORT error as "assume
/// supported") is the oracle's own behavior (`parent_issue.zig`'s
/// `detectParentIssueSupport`), preserved rather than tightened.
///
/// The write is best-effort: when the anchor has no mirror link row yet
/// (the very first call, before `propagate_parent_issue_with_repo` has
/// written one), the write is a silent no-op — the first `create` call
/// establishes the row and its `config_json` in the same pass.
/// @param conn An open, migrated connection.
/// @param client The GitHub client to probe with.
/// @param owner The repo owner.
/// @param repo The repo name.
/// @param anchor_plan_id The anchor plan.
/// @param system_id The registered system.
/// @return Whether sub-issues are supported, or the query failure.
export auto detect_parent_issue_support(db::connection& conn, gh_client& client, std::string_view owner, std::string_view repo,
                                        std::int64_t anchor_plan_id, std::int64_t system_id)
    -> std::expected<bool, parent_issue_error>;

/// @brief Run the single-repo parent-issue flow against an explicit
/// owner/repo.
///
/// Creates (or skips, when already linked) the anchor as the parent
/// issue, each child plan as a sub-issue of the parent, each task under a
/// child plan as a sub-issue of that child, each task attached directly
/// to the anchor as a sub-issue of the parent, and — best-effort, only on
/// a real (non-dry-run) run whose parent issue was actually created or
/// already existed — every decision linked to the anchor as a comment on
/// the parent issue.
///
/// **Already-exists behavior**: `create_or_skip_github_issue` checks
/// `link::load_existing_mirror` BEFORE calling any `gh_client` method. A
/// repeat run therefore sends ZERO requests for every already-linked
/// entity — the same "skip, zero requests" shape `ext propagate-one`
/// implements, and a different shape from `ext create` (re-POSTs a
/// duplicate, a recorded defect) and `workbench publish` (refuses).
///
/// Under `opts.dry_run`, no `gh_client` method is ever called and no
/// database write happens — every not-yet-linked entity is reported
/// `op::created` with `external_id == "(dry-run)"`.
/// @param conn An open, migrated connection.
/// @param client The GitHub client.
/// @param anchor_plan_id The anchor plan.
/// @param owner The target repo owner.
/// @param repo The target repo name.
/// @param options Propagate options.
/// @return The report, or the failure. `sub_issue_unsupported` is
/// returned (before any write) when the sub-issue probe 404s and
/// `opts.dry_run` is false.
export auto propagate_parent_issue_with_repo(db::connection& conn, gh_client& client, std::int64_t anchor_plan_id,
                                             std::string_view owner, std::string_view repo, const opts& options)
    -> std::expected<report, parent_issue_error>;

/// @brief `propagate_parent_issue_with_repo` with the target repo resolved
/// from the feature's touched-repo set.
/// @param conn An open, migrated connection.
/// @param client The GitHub client.
/// @param anchor_plan_id The anchor plan.
/// @param options Propagate options.
/// @return The report, or the failure (including `resolve_target_repo`'s).
export auto propagate_parent_issue(db::connection& conn, gh_client& client, std::int64_t anchor_plan_id, const opts& options)
    -> std::expected<report, parent_issue_error>;

/// @brief `propagate_parent_issue_with_repo` with the owner/repo parsed
/// out of a `"owner/repo"`-shaped `github_lead_repo` config value.
/// @param conn An open, migrated connection.
/// @param client The GitHub client.
/// @param anchor_plan_id The anchor plan.
/// @param lead_repo The `github_lead_repo` config value.
/// @param options Propagate options.
/// @return The report, or `bad_config` when `lead_repo` is empty or has no
/// non-empty owner and repo either side of exactly one `/`.
export auto propagate_zero_repo(db::connection& conn, gh_client& client, std::int64_t anchor_plan_id, std::string_view lead_repo,
                                const opts& options) -> std::expected<report, parent_issue_error>;

} // namespace planar::engine::external::parent_issue
